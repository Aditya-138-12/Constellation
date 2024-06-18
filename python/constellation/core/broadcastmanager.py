"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: CC-BY-4.0

BroadcastManger module provides classes for managing CHIRP broadcasts within
Constellation Satellites.
"""

import logging
import threading
from functools import wraps
from typing import Callable

import time
from uuid import UUID
from queue import Queue

from .base import BaseSatelliteFrame

from .chirp import (
    CHIRPServiceIdentifier,
    CHIRPMessage,
    CHIRPMessageType,
    CHIRPBeaconTransmitter,
)


def chirp_callback(request_service: CHIRPServiceIdentifier):
    """Mark a function as a callback for CHIRP service requests."""

    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            return func(*args, **kwargs)

        # mark function as chirp callback
        wrapper.chirp_callback = request_service
        return wrapper

    return decorator


def get_chirp_callbacks(cls):
    """Loop over all class methods and return those marked as CHIRP callback."""
    res = {}
    for func in dir(cls):
        if isinstance(getattr(type(cls), func, None), property):
            # skip properties
            continue
        call = getattr(cls, func)
        if callable(call) and not func.startswith("__"):
            # regular method
            if hasattr(call, "chirp_callback"):
                res[getattr(call, "chirp_callback")] = call
    return res


class DiscoveredService:
    """Class to hold discovered service data."""

    def __init__(
        self,
        host_uuid: UUID,
        serviceid: CHIRPServiceIdentifier,
        address,
        port: int,
        alive: bool = True,
    ):
        """Initialize variables."""
        self.host_uuid = host_uuid
        self.address = address
        self.port = port
        self.serviceid = serviceid
        self.alive = alive

    def __eq__(self, other):
        """Comparison operator for network-related properties."""
        return (
            self.host_uuid == other.host_uuid
            and self.serviceid == other.serviceid
            and self.port == other.port
        )

    def __str__(self):
        """Pretty-print a string for this service."""
        s = "Host {} offering service {} on {}:{} is alive: {}"
        return s.format(
            self.host_uuid, self.serviceid, self.address, self.port, self.alive
        )


class CHIRPBroadcaster(BaseSatelliteFrame):
    """Manages service discovery and broadcast via the CHIRP protocol.

    Listening and reacting to CHIRP broadcasts is implemented in a dedicated
    thread that can be started after the class has been instantiated.

    Discovered services are added to an internal cache. Callback methods can be
    registered either by calling register_request() or by using the
    @chirp_callback() decorator. The callback will be added to the satellite's
    internal task queue once the corresponding service has been offered by other
    satellites via broadcast.

    Offered services can be registered via register_offer() and are announced on
    incoming request broadcasts or via broadcast_offers().

    """

    def __init__(
        self,
        name: str,
        group: str,
        interface: str,
        **kwds,
    ):
        """Initialize parameters.

        :param host: Satellite name the BroadcastManager represents
        :type host: str
        :param group: group the Satellite belongs to
        :type group: str
        """
        super().__init__(name=name, interface=interface, **kwds)
        self._stop_broadcasting = threading.Event()
        self._beacon = CHIRPBeaconTransmitter(self.name, group, interface)

        # Offered and discovered services
        self._registered_services: dict[int, CHIRPServiceIdentifier] = {}
        self.discovered_services: list[DiscoveredService] = []
        self._chirp_thread = None
        self._chirp_callbacks = get_chirp_callbacks(self)

    def _add_com_thread(self):
        """Add the CHIRP broadcaster thread to the communication thread pool."""
        super()._add_com_thread()
        self._com_thread_pool["chirp_broadcaster"] = threading.Thread(
            target=self._run, daemon=True
        )
        self.log.debug("CHIRP broadcaster thread prepared and added to the pool.")

    def get_discovered(
        self, serviceid: CHIRPServiceIdentifier
    ) -> list[DiscoveredService]:
        """Return a list of already discovered services for a given identifier."""
        res = []
        for s in self.discovered_services:
            if s.serviceid == serviceid:
                res.append(s)
        return res

    def register_request(
        self, serviceid: CHIRPServiceIdentifier, callback: Callable
    ) -> None:
        """Register new callback for ServiceIdentifier."""
        if serviceid in self._chirp_callbacks:
            self.log.warning("Overwriting CHIRP callback")
        self._chirp_callbacks[serviceid] = callback
        # make a callback if a service has already been discovered
        for known in self.get_discovered(serviceid):
            self.task_queue.put((callback, [known]))

    def register_offer(self, serviceid: CHIRPServiceIdentifier, port: int) -> None:
        """Register new offered service or overwrite existing service."""
        if port in self._registered_services:
            self.log.warning("Replacing service registration for port %d", port)
        self._registered_services[port] = serviceid

    def request(self, serviceid: CHIRPServiceIdentifier) -> None:
        """Request specific service.

        Should already have a registered callback via register_request(), or
        any incoming OFFERS will go unnoticed.

        """
        if serviceid not in self._chirp_callbacks:
            self.log.warning(
                "Serviceid %s does not have a registered callback", serviceid
            )
        self._beacon.broadcast(serviceid, CHIRPMessageType.REQUEST)

    def broadcast_offers(self, serviceid: CHIRPServiceIdentifier | None = None) -> None:
        """Broadcast all registered services matching serviceid.

        Specify None for all registered services.

        """
        for port, sid in self._registered_services.items():
            if not serviceid or serviceid == sid:
                self.log.debug("Broadcasting service OFFER on %s for %s", port, sid)
                self._beacon.broadcast(sid, CHIRPMessageType.OFFER, port)

    def broadcast_requests(self) -> None:
        """Broadcast all requests registered via register_request()."""
        for serviceid in self._chirp_callbacks:
            self.log.debug("Broadcasting service REQUEST for %s", serviceid)
            self._beacon.broadcast(serviceid, CHIRPMessageType.REQUEST)

    def broadcast_depart(self) -> None:
        """Broadcast DEPART for all registered services."""
        for port, sid in self._registered_services.items():
            self.log.debug("Broadcasting service DEPART on %d for %s", port, sid)
            self._beacon.broadcast(sid, CHIRPMessageType.DEPART, port)

    def _discover_service(self, msg: CHIRPMessage) -> None:
        """Add a service to internal list and possibly queue a callback."""
        service = DiscoveredService(
            msg.host_uuid, msg.serviceid, msg.from_address, msg.port
        )
        if service in self.discovered_services:
            self.log.debug(
                "Service already discovered: %s on host %s",
                msg.serviceid,
                msg.from_address,
            )
            # NOTE we might want to call the callback method for this service
            # anyway, in case this host was down (without sending DEPART) and is
            # now reconnecting. But then the bookkeeping has to be done higher up.
        else:
            # add service to internal list and queue callback (if registered)
            self.log.info(
                "Received new OFFER for service: %s on host %s:%s",
                msg.serviceid.name,
                msg.from_address,
                msg.port,
            )
            try:
                callback = self._chirp_callbacks[msg.serviceid]
                self.task_queue.put((callback, [service]))
            except KeyError:
                self.log.debug("No callback for service %s set up.", msg.serviceid)
            self.discovered_services.append(service)

    def _depart_service(self, msg: CHIRPMessage) -> None:
        """Depart with a service."""
        try:
            service = DiscoveredService(
                msg.host_uuid, msg.serviceid, msg.from_address, msg.port
            )
            self.discovered_services.remove(service)
            self.log.debug(
                "Received depart for service %s on host %s: Removed.",
                msg.serviceid,
                msg.from_address,
            )
            # indicate that service is no longer with us
            service.alive = False
            try:
                callback = self._chirp_callbacks[msg.serviceid]
                self.task_queue.put((callback, [service]))
            except KeyError:
                self.log.debug("No callback for service %s set up.", msg.serviceid)
        except ValueError:
            self.log.debug(
                "Received depart for service %s on host %s: Not in use.",
                msg.serviceid,
                msg.from_address,
            )

    def _run(self) -> None:
        """Start listening in on broadcast"""
        # assert for mypy static type analysis
        assert isinstance(
            self._com_thread_evt, threading.Event
        ), "BroadcastManager thread Event no set up"

        while not self._com_thread_evt.is_set():
            msg = self._beacon.listen()
            if not msg:
                time.sleep(0.1)
                continue

            # Check Message Type
            if msg.msgtype == CHIRPMessageType.REQUEST:
                self.broadcast_offers(msg.serviceid)
                continue

            if msg.msgtype == CHIRPMessageType.OFFER:
                self._discover_service(msg)
                continue

            if msg.msgtype == CHIRPMessageType.DEPART and msg.port != 0:
                self._depart_service(msg)
                continue
        # shutdown
        self.log.info("BroadcastManager thread shutting down.")
        self.broadcast_depart()
        # it can take a moment for the network buffers to be flushed
        time.sleep(0.5)
        self._beacon.close()


def main(args=None):
    """Start a broadcast manager service."""
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--log-level", default="info")

    args = parser.parse_args(args)
    logging.basicConfig(
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        level=args.log_level.upper(),
    )

    q = Queue()
    # start server with remaining args
    bcst = CHIRPBroadcaster(
        name="broadcast_test",
        group="Constellation",
        callback_queue=q,
    )
    bcst.register_offer(CHIRPServiceIdentifier.HEARTBEAT, 50000)
    bcst.register_offer(CHIRPServiceIdentifier.CONTROL, 50001)
    print("Services registered")
    bcst.broadcast_offers()
    print("Broadcast for offers sent")
    bcst.start()
    print("Listener started, sleeping...")
    time.sleep(10)
    bcst.stop()


if __name__ == "__main__":
    main()
