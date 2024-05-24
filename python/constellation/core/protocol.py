#!/usr/bin/env python3
"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: CC-BY-4.0

Module implementing the Constellation communication protocols.
"""

import msgpack
import zmq

import io
import time
from enum import StrEnum


class Protocol(StrEnum):
    CDTP = "CDTP\x01"
    CSCP = "CSCP\x01"
    CMDP = "CMDP\x01"
    CHP = "CHP\x01"


class MessageHeader:
    """Class implementing a Constellation message header."""

    def __init__(self, name: str, protocol: Protocol):
        self.name = name
        self.protocol = protocol

    def send(
        self, socket: zmq.Socket, flags: int = zmq.SNDMORE, meta: dict = None, **kwargs
    ):
        """Send a message header via socket.

        meta is an optional dictionary that is sent as a map of string/value
        pairs with the header.

        Returns: return value from socket.send().

        """
        return socket.send(self.encode(meta, **kwargs), flags)

    def recv(self, socket: zmq.Socket, flags: int = 0):
        """Receive header from socket and return all decoded fields."""
        return self.decode(socket.recv())

    def decode(self, header):
        """Decode header string and return host, timestamp and meta map."""
        unpacker = msgpack.Unpacker()
        unpacker.feed(header)
        protocol = unpacker.unpack()
        if not protocol == self.protocol.value:
            raise RuntimeError(
                f"Received message with malformed {self.protocol.name} header: {header}!"
            )
        host = unpacker.unpack()
        timestamp = unpacker.unpack()
        if protocol == Protocol.CDTP:
            msgtype = unpacker.unpack()
            seqno = unpacker.unpack()
            meta = unpacker.unpack()
            return host, timestamp, msgtype, seqno, meta
        meta = unpacker.unpack()
        return host, timestamp, meta

    def encode(self, meta: dict = None, **kwargs):
        """Generate and return a header as list.

        Additional keyword arguments are required for protocols specifying
        additional fields.

        """
        if not meta:
            meta = {}
        stream = io.BytesIO()
        packer = msgpack.Packer()
        stream.write(packer.pack(self.protocol.value))
        stream.write(packer.pack(self.name))
        stream.write(packer.pack(msgpack.Timestamp.from_unix_nano(time.time_ns())))
        if self.protocol == Protocol.CDTP:
            stream.write(packer.pack(kwargs["msgtype"]))
            stream.write(packer.pack(kwargs["seqno"]))
        stream.write(packer.pack(meta))
        return stream.getbuffer()
