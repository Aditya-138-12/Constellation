"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: EUPL-1.2

This module provides classes for managing CSCP requests/replies within
Constellation Satellites.
"""

import threading
import time
from functools import wraps
from typing import Any, Callable, ParamSpec, Tuple, TypeVar

import zmq
from statemachine.exceptions import TransitionNotAllowed

from .base import BaseSatelliteFrame
from .cscp import CommandTransmitter, CSCPMessage, CSCPMessageVerb

T = TypeVar("T")
P = ParamSpec("P")


def cscp_requestable(func: Callable[P, T]) -> Callable[P, T]:
    """Register a function as a supported command for CSCP.

    See CommandReceiver for a description of the expected signature.

    """

    @wraps(func)
    def wrapper(*args: P.args, **kwargs: P.kwargs) -> T:
        return func(*args, **kwargs)

    # mark function as chirp callback
    wrapper.cscp_command = True  # type: ignore[attr-defined]
    return wrapper


def get_cscp_commands(cls: Any) -> dict[str, str]:
    """Loop over all class methods and return those marked as CSCP commands."""
    res = {}
    for func in dir(cls):
        if isinstance(getattr(type(cls), func, None), property):
            # skip properties
            continue
        call = getattr(cls, func)
        if callable(call) and not func.startswith("__"):
            # regular method
            if hasattr(call, "cscp_command"):
                doc = call.__doc__
                res[func] = doc
    return res


class CommandReceiver(BaseSatelliteFrame):
    """Class for handling incoming CSCP requests.

    Commands will call specific methods of the inheriting class which should
    have the following signature:

    def COMMAND(self, request: cscp.CSCPMessage) -> (str, any, dict):

    The expected return values are:
    - reply message (string)
    - payload (any)
    - map (dictionary) (e.g. for meta information)

    Inheriting classes need to decorate such command methods with
    '@cscp_requestable' to make them callable through CSCP requests.

    If a method

    def _COMMAND_is_allowed(self, request: cscp.CSCPMessage) -> bool:

    exists, it will be called first to determine whether the command is
    currently allowed or not.

    """

    def __init__(self, name: str, cmd_port: int, interface: str, **kwds: Any):
        """Initialize the Receiver and set up a ZMQ REP socket on given port."""
        super().__init__(name=name, interface=interface, **kwds)

        self.log_cscp = self.get_logger("CSCP")

        # set up the command channel
        sock = self.context.socket(zmq.REP)
        if not cmd_port:
            self.cmd_port = sock.bind_to_random_port(f"tcp://{interface}")
        else:
            sock.bind(f"tcp://{interface}:{cmd_port}")
            self.cmd_port = cmd_port

        self.log_cscp.info(f"Satellite listening on command port {self.cmd_port}")
        self._cmd_tm = CommandTransmitter(self.name, sock)
        # cached list of supported commands
        self._cmds = get_cscp_commands(self)

    def _add_com_thread(self) -> None:
        """Add the command receiver thread to the communication thread pool."""
        super()._add_com_thread()
        self._com_thread_pool["cmd_receiver"] = threading.Thread(target=self._recv_cmds, daemon=True)
        self.log_cscp.debug("Command receiver thread prepared and added to the pool.")

    def _recv_cmds(self) -> None:
        """Request receive loop."""
        # assert for mypy static type analysis
        assert isinstance(self._com_thread_evt, threading.Event), "Thread Event not set up correctly"
        while not self._com_thread_evt.is_set():
            try:
                req = self._cmd_tm.get_message(flags=zmq.NOBLOCK)
            except zmq.ZMQError as e:
                # something wrong with the ZMQ socket, wait a while for recovery
                self.log_cscp.exception(e)
                time.sleep(0.5)
                continue
            if not req:
                # no message waiting for us, rest until next attempt
                time.sleep(0.025)
                continue
            # check that it is actually a REQUEST
            if req.msg_verb != CSCPMessageVerb.REQUEST:
                self.log_cscp.error(f"Received malformed request with msg verb: {req.msg_verb}")
                self._cmd_tm.send_reply(
                    f"Received malformed request with msg verb: {req.msg_verb}",
                    CSCPMessageVerb.INVALID,
                )
                continue

            # find a matching callback
            if req.msg not in self._cmds:
                self.log_cscp.error("Unknown command: %s", req)
                self._cmd_tm.send_reply(f"Unknown command: {req.msg}", CSCPMessageVerb.UNKNOWN)
                continue
            # test whether callback is allowed by calling the
            # method "_COMMAND_is_allowed" (if exists).
            try:
                is_allowed = getattr(self, f"_{req.msg}_is_allowed")(req)
                if not is_allowed:
                    self.log_cscp.error("Command not allowed: %s", req)
                    self._cmd_tm.send_reply(
                        "Command not allowed (in current state)",
                        CSCPMessageVerb.INVALID,
                    )
                    continue
            except AttributeError:
                pass
            # perform the actual callback
            try:
                self.log_cscp.debug("Calling command %s with argument %s", req.msg, req)
                res, payload, meta = getattr(self, req.msg)(req)
            except (AttributeError, NotImplementedError) as e:
                self.log_cscp.error("Command failed with %s: %s", e, req)
                self._cmd_tm.send_reply(
                    f"WrongImplementation: {repr(e)}",
                    CSCPMessageVerb.NOTIMPLEMENTED,
                    repr(e),
                )
                continue
            except TransitionNotAllowed as e:
                self.log_cscp.error("Transition '%s' not allowed: %s", req.msg, e)
                self._cmd_tm.send_reply(f"Transition not allowed: {e}", CSCPMessageVerb.INVALID, repr(e))
                continue
            except (TypeError, ValueError) as e:
                self.log_cscp.error("Command '%s' received wrong argument: %s", req.msg, repr(e))
                self._cmd_tm.send_reply(f"Wrong argument: {repr(e)}", CSCPMessageVerb.INCOMPLETE, repr(e))
                continue
            except Exception as e:
                self.log_cscp.error("Command '%s' failed: %s", req.msg, repr(e))
                self._cmd_tm.send_reply(f"Exception: {repr(e)}", CSCPMessageVerb.INVALID, repr(e))
                continue
            # check the response; empty string means 'missing data/incomplete'
            if res is None:
                self.log_cscp.error("Command returned nothing: %s", req)
                self._cmd_tm.send_reply("Command returned nothing", CSCPMessageVerb.INCOMPLETE)
                continue
            # finally, assemble a proper response!
            self.log_cscp.debug("Command succeeded with '%s': %s", res, req)
            try:
                self._cmd_tm.send_reply(res, CSCPMessageVerb.SUCCESS, payload, meta)
            except TypeError as e:
                self.log_cscp.exception("Sending response '%s' failed: %s", res, e)
                self._cmd_tm.send_reply(str(e), CSCPMessageVerb.ERROR, None, None)
        self.log_cscp.info("CommandReceiver thread shutting down.")
        # shutdown
        self._cmd_tm.socket.close()

    @cscp_requestable
    def get_commands(self, _request: CSCPMessage | None = None) -> Tuple[str, dict[str, str], None]:
        """Return all commands supported by the Satellite.

        No payload argument.

        This will include all methods with the @cscp_requestable decorator. The
        doc string of the function will be used to derive the summary and
        payload argument description for each command by using the first and the
        second line of the doc string, respectively (not counting empty lines).

        """
        public_cmds = {}
        for key, value in self._cmds.items():
            if not key.startswith("_"):
                public_cmds[key] = value
        return f"{len(public_cmds)} commands known", public_cmds, None

    @cscp_requestable
    def _get_commands(self, _request: CSCPMessage | None = None) -> Tuple[str, dict[str, str], None]:
        """Return all hidden commands supported by the Satellite.

        No payload argument.

        This will include all methods with the @cscp_requestable decorator starting with an underscore. The
        doc string of the function will be used to derive the summary and payload argument description for
        each command by using the first and the second line of the doc string, respectively (not counting
        empty lines).

        """
        hidden_cmds = {}
        for key, value in self._cmds.items():
            if key.startswith("_"):
                hidden_cmds[key] = value
        return f"{len(hidden_cmds)} commands known", hidden_cmds, None

    @cscp_requestable
    def get_name(self, _request: CSCPMessage) -> Tuple[str, None, None]:
        """Return the canonical name of the Satellite.

        No payload argument.

        """
        return self.name, None, None

    @cscp_requestable
    def shutdown(self, _request: CSCPMessage) -> Tuple[str, None, None]:
        """Queue the Satellite's reentry.

        No payload argument.

        """

        # initialize shutdown with delay (so that CSCP response reaches
        # Controller)
        def reentry_timer(sat: BaseSatelliteFrame) -> None:
            time.sleep(0.5)
            import sys

            sys.exit(0)

        # This command is put into the queue: it will only execute after
        # previously queued actions (e.g. state transitions) have been
        # completed.
        self.task_queue.put((reentry_timer, [self]))
        return f"{self.name} queued for reentry", None, None
