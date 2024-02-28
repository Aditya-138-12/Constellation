#!/usr/bin/env python3
"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: CC-BY-4.0

This module provides the class for a Constellation Satellite.
"""

import traceback
import time
import threading
from queue import Queue, Empty
import logging

from .fsm import SatelliteFSM
from .heartbeater import Heartbeater
from .heartbeatchecker import HeartbeatChecker

from .cscp import CSCPMessage
from .chirp import CHIRPServiceIdentifier
from .broadcastmanager import CHIRPBroadcastManager
from .commandmanager import CommandReceiver, cscp_requestable
from .log_and_stats import getLoggerAndStats
from .error import debug_log, handle_error
from ._version import version


class IncompleteCommand(Exception):
    pass


class Satellite(CommandReceiver):
    """Base class for a Constellation Satellite."""

    def __init__(
        self, name: str, group: str, cmd_port: int, hb_port: int, log_port: int
    ):
        """Set up class attributes."""
        super().__init__(
            name=name,
            group=group,
            cmd_port=cmd_port,
            hb_port=hb_port,
            log_port=log_port,
        )

        # set up python logging
        self.log, self.stats = getLoggerAndStats(self.name, self.context, log_port)

        # state machine
        self.fsm = SatelliteFSM()
        # Adding this class as observer to the FSM allows class-internal methods such as
        # on_start to be Actions to be performed on a Transition of the state
        # machine. This will ensure that the state does not change before we
        # have performed all necessary steps.
        self.fsm.add_observer(self)

        # Set up a queue for handling tasks related to incoming requests via
        # CSCP or offers via CHIRP. This makes sure that these can be performed
        # thread-safe.
        self.task_queue = Queue()

        # set up background communication threads
        # NOTE should be a late part of the initialization, as it starts communication
        super()._add_com_thread()
        super()._start_com_threads()

        # register and start heartbeater
        self.heartbeater = Heartbeater(
            self.get_state, f"tcp://*:{hb_port}", context=self.context
        )
        self.heartbeater.start()

        # register heartbeat checker
        self.hb_checker = HeartbeatChecker()

        # register broadcast manager
        self.broadcast_manager = CHIRPBroadcastManager(name, group, self.task_queue)
        self.broadcast_manager.start()
        self.broadcast_manager.register_offer(cmd_port, CHIRPServiceIdentifier.CONTROL)
        self.log.info("Satellite broadcasting CONTROL service")
        self.broadcast_manager.register_offer(hb_port, CHIRPServiceIdentifier.HEARTBEAT)
        self.log.info("Satellite broadcasting HEARTBEAT service")
        self.broadcast_manager.register_offer(
            log_port, CHIRPServiceIdentifier.MONITORING
        )
        self.log.info("Satellite broadcasting MONITORING service")
        # acquisition thread
        self._stop_running = None
        self._running_thread = None

        # Add exception handling via threading.excepthook to allow the state
        # machine to reflect exceptions in the receiver thread.
        #
        # NOTE: This approach using a global state hook does not play well e.g.
        # with the default pytest configuration, however (see
        # https://github.com/pytest-dev/pytest/discussions/9193). Without
        # disabling the threadexception plugin, tests of the exception handling
        # will fail with pytest.
        threading.excepthook = self._thread_exception
        # greet
        self.log.info(f"Satellite {self.name}, version {self.version} ready to launch!")

    def _transition_is_allowed(self, request: CSCPMessage):
        """Determine whether a requested transition is allowed.

        This method will be called by the CommandReceiver to determine whether
        to complete a request by a call to transition() or to deny the request.

        """
        args = request.payload.split()
        transition_target = args[0].lower()
        # is transition allowed?
        if not getattr(self.fsm, transition_target) in self.fsm.allowed_events:
            return False
        # TODO check that we are not transitioning away from a transitional state (not allowed)
        return True

    @cscp_requestable
    def transition(self, request: CSCPMessage):
        """Queue a state transition via a CSCP request."""
        args = request.payload.split()
        transition_target = args[0].capitalize()
        callback = getattr(self, transition_target)
        # add to the task queue
        self.task_queue.put((callback, args[1:]))
        return "transitioning", transition_target, None

    @cscp_requestable
    def configure(self, request: CSCPMessage):
        """Update configuration via CSCP request."""
        config = request.payload
        callback = self.update_config
        # add to the task queue
        self.task_queue.put((callback, config))
        return "configuring", None, None

    @cscp_requestable
    def reload(self, request: CSCPMessage):
        """Reload configuration."""
        callback = self.set_config
        self.task_queue.put((callback, None))
        return "reloading", None, None

    @cscp_requestable
    def register(self, request: CSCPMessage):
        """Register a heartbeat via CSCP request."""
        name, ip, port = request.payload.split()
        callback = self.hb_checker.register
        # add to the task queue
        self.task_queue.put((callback, [name, f"tcp://{ip}:{port}", self.context]))
        return "registering", name, None

    def run_satellite(self):
        """Main event loop with task handler-routine"""
        while True:
            # TODO: add check for heartbeatchecker: if any entries in hb.get_failed, trigger action

            try:
                # blocking call but with timeout to prevent deadlocks
                task = self.task_queue.get(block=True, timeout=0.5)
                callback = task[0]
                args = task[1]
                try:
                    callback(*args)
                except Exception as e:
                    # TODO consider whether to go into error state if anything goes wrong here
                    self.log.error("Caught exception handling task: %s", repr(e))
            except Empty:
                # nothing to process
                pass

            time.sleep(1)
        # TODO add a 'finally:' which closes zmq context via .term() and cleans up other things
        # on exit: stop heartbeater
        self.heartbeater.stop()
        # TODO : shutdown broadcast manager and depart

    # --------------------------- #
    # ----- satellite commands ----- #
    # --------------------------- #

    @handle_error
    @debug_log
    def Initialize(self):
        """Initialize.

        Actual actions will be performed by the callback method 'on_load'
        as long as the transition is allowed.
        """
        self.fsm.initialize("Satellite initialized.")
        self.log.info("Satellite Initialized.")

    @handle_error
    def on_initialize(self):
        """Callback method for the 'initialize' transition of the FSM.

        Set and check config, maybe initialize device.
        """
        # TODO on_initialize should (re-)load config values
        #
        # Verify that there are no running threads left. If there are and the
        # timeout is exceeded joining them, the raised exception will take us
        # into ERROR state.
        self._stop_daq_thread(10.0)

    @handle_error
    @debug_log
    def Launch(self):
        """Prepare Satellite for data acquistions.

        Actual actions will be performed by the callback method 'on_launch'
        aslong as the transition is allowed.
        """
        self.fsm.launch("Satellite launched.")
        self.hb_checker.start()

        self.log.info("Satellite Prepared. Acquistion ready.")

    @handle_error
    def on_launch(self):
        """Callback method for the 'launch' transition of the FSM."""
        pass

    @handle_error
    @debug_log
    def Land(self):
        """Return Satellite to Initialized state.

        Actual actions will be performed by the callback method 'on_land'
        aslong as the transition is allowed.
        """
        self.fsm.land("Satellite landed.")
        self.hb_checker.stop()
        self.log.info("Satellite landed.")

    @handle_error
    def on_land(self):
        """Callback method for the 'unprepare' transition of the FSM."""
        pass

    @handle_error
    @debug_log
    def Start(self):
        """Start command to begin data acquisition.

        Actual Satellite-specific actions will be performed by the callback
        method 'on_start' as long as the transition is allowed.

        """
        self.fsm.start("Acquisition started.")
        # start thread running during acquistion
        self._stop_running = threading.Event()
        self._running_thread = threading.Thread(target=self.do_run, daemon=True)
        self._running_thread.start()
        self.log.info("Satellite Running. Acquistion taking place.")

    @handle_error
    def on_start(self):
        """Callback method for the 'start_run' transition of the FSM.

        Is called *before* the 'do_run' thread is started.
        """
        pass

    @handle_error
    @debug_log
    def Stop(self):
        """Stop command stopping data acquisition.

        Actual actions will be performed by the callback method 'on_stop_run'
        aslong as the transition is allowed.
        """
        self.fsm.stop("Acquisition stopped.")
        self._stop_daq_thread()
        self.log.info("Satellite stopped Acquistion.")

    @handle_error
    def on_stop(self):
        """Callback method for the 'stop_run' transition of the FSM.

        This method is called *before* the 'do_run' is stopped.
        """
        pass

    def do_run(self):
        """The acquisition event loop.

        This method will be started by the Satellite and run in a thread. It
        therefore needs to monitor the self.stop_running Event and close itself
        down if the Event is set.

        NOTE: This method is not inherently thread-safe as it runs in the
        context of the Satellite and can modify data accessible to the main
        thread. However, the state machine can effectively act as a lock and
        prevent competing access to the same objects while in RUNNING state as
        long as care is taken in the implementation.

        The state machine itself uses the RTC model by default (see
        https://python-statemachine.readthedocs.io/en/latest/processing_model.html?highlight=thread)
        which should make the transitions themselves safe.

        """
        # the stop_running Event will be set from outside the thread when it is
        # time to close down.
        while not self._stop_running.is_set():
            time.sleep(0.2)

    @handle_error
    @debug_log
    def Failure(self, message: str = None):
        """Trigger a failure on Satellite.

        This is a "command" and will only be called by user action.

        Actual actions will be performed by the callback method 'on_failure'
        which is called automatically whenever a failure occurs.

        """
        self.log.error(f"Failure action was triggered with reason given: {message}.")
        self.fsm.failure(message)

    def on_failure(self):
        """Callback method for the 'on_failure' transition of the FSM

        This method should implement Satellite-specific actions in case of
        failure.

        """
        try:
            self._stop_daq_thread(10.0)
            # Stop heartbeat checking
            self.hb_checker.stop()
        # NOTE: we cannot have a non-handled exception disallow the state
        # transition to failure state!
        except Exception as e:
            self.log.exception(e)

    @handle_error
    @debug_log
    def Interrupt(self, message: str = None):
        """Interrupt data acquisition and move to Safe state.

        Actual actions will be performed by the callback method 'on_interrupt'
        aslong as the transition is allowed.
        """
        self.fsm.interrupt(message)
        self.log.warning("Transitioned to Safe state.")

    @handle_error
    def on_interrupt(self):
        """Callback method for the 'on_interrupt' transition of the FSM.

        Defaults to calling on_failure().
        """
        self.on_failure()

    @handle_error
    @debug_log
    def Recover(self):
        """Transition to Initialized state.

        Actual actions will be performed by the callback method 'on_recover'
        aslong as the transition is allowed.
        """
        self.fsm.recover("Recovered from Safe state.")
        self.log.info("Recovered from Safe state.")

    @handle_error
    def on_recover(self):
        """Callback method for the 'on_recover' transition of the FSM.

        Defaults to on_initialize().
        """
        self.on_initialize()

    def _stop_daq_thread(self, timeout: float = 30.0):
        """Stop the acquisition thread.

        Raises RuntimeError if thread is not stopped within timeout."""
        if self._running_thread and self._running_thread.is_alive():
            self._stop_running.set()
            self._running_thread.join(timeout)
        # check if thread is still alive
        if self._running_thread and self._running_thread.is_alive():
            raise RuntimeError(
                f"Could not join running thread within timeout of {timeout}s!"
            )
        self._running_thread = None
        self._stop_running = None

    def _thread_exception(self, args):
        """Handle exceptions in threads.

        Change state to FAULT.

        Intended to be installed as threading.excepthook.

        """
        tb = "".join(traceback.format_tb(args.exc_traceback))
        self.log.fatal(
            f"caught {args.exc_type} with value \
            {args.exc_value} in thread {args.thread} and traceback {tb}."
        )
        # indicate to remaining thread to stop
        if self._stop_running:
            self._stop_running.set()
        # change internal state
        self.fsm.failure(
            f"Thread {args.thread} failed. Caught exception {args.exc_type} \
            with value {args.exc_value}."
        )

    # -------------------------- #
    # ----- device methods ----- #
    # -------------------------- #

    @cscp_requestable
    def get_state(self, _request: CSCPMessage = None) -> str:
        return self.fsm.current_state.id, None, None

    @cscp_requestable
    def get_status(self, _request: CSCPMessage = None) -> str:
        return self.fsm.status, None, None

    @cscp_requestable
    def version(self, _request: CSCPMessage = None):
        """Get Constellation version."""
        return version, None, None


# -------------------------------------------------------------------------


def main(args=None):
    """Start the base Satellite server."""
    import argparse

    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--log-level", default="info")
    parser.add_argument("--cmd-port", type=int, default=23999)
    parser.add_argument("--log-port", type=int, default=5556)  # Should be 55556?
    parser.add_argument("--hb-port", type=int, default=61234)
    parser.add_argument("--name", type=str, default="satellite_demo")
    parser.add_argument("--group", type=str, default="constellation")
    args = parser.parse_args(args)

    # set up logging
    logger = logging.getLogger()  # get root logger
    formatter = logging.Formatter(
        "%(asctime)s | %(name)s |  %(levelname)s: %(message)s"
    )
    # global level should be the lowest level that we want to see on any
    # handler, even streamed via ZMQ
    logger.setLevel(0)

    stream_handler = logging.StreamHandler()
    stream_handler.setLevel(args.log_level.upper())
    stream_handler.setFormatter(formatter)
    logger.addHandler(stream_handler)

    logger.info("Starting up satellite!")
    # start server with remaining args
    s = Satellite(args.name, args.group, args.cmd_port, args.hb_port, args.log_port)
    s.run_satellite()


if __name__ == "__main__":
    main()
