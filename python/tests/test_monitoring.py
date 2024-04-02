"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: CC-BY-4.0
"""

import pytest
import logging
import time
from unittest.mock import MagicMock, patch

from constellation.cmdp import CMDPTransmitter, Metric, MetricsType

from constellation.monitoring import (
    ZeroMQSocketLogListener,
    MonitoringSender,
    schedule_metric,
)

from conftest import mock_packet_queue_sender, mocket, send_port


@pytest.fixture
def mock_transmitter_a():
    """Mock Transmitter endpoint A."""
    m = mocket()
    m.port = send_port
    cmdp = CMDPTransmitter("mock_cmdp", m)
    yield cmdp, m


@pytest.fixture
def mock_transmitter_b(mock_socket_sender):
    """Mock Transmitter endpoint B."""
    cmdp = CMDPTransmitter("mock_cmdp", mock_socket_sender)
    yield cmdp


@pytest.fixture
def mock_monitoringsender():
    """Create a mock MonitoringManager instance."""

    class MyStatProducer(MonitoringSender):
        @schedule_metric(MetricsType.LAST_VALUE, 0.1)
        def get_answer(self):
            """The answer to the Ultimate Question"""
            return 42, "Answer"

    def mocket_factory(*args, **kwargs):
        m = mocket()
        return m

    with patch("constellation.base.zmq.Context") as mock:
        mock_context = MagicMock()
        mock_context.socket = mocket_factory
        mock.return_value = mock_context
        m = MyStatProducer("mock_monitor", send_port, interface="127.0.0.1")
        yield m


@pytest.fixture
def mock_listener(mock_transmitter_b):
    """Create a mock log listener instance."""

    mock_handler = MagicMock()
    mock_handler.handle.return_value = None
    mock_handler.emit.return_value = None
    mock_handler.create_lock.return_value = None
    mock_handler.lock = None
    listener = ZeroMQSocketLogListener(mock_transmitter_b, mock_handler)
    yield listener, mock_handler


@pytest.mark.forked
def test_log_transmission(mock_transmitter_a, mock_transmitter_b):
    cmdp, m = mock_transmitter_a
    log = logging.getLogger()
    rec1 = log.makeRecord("name", 10, __name__, 42, "mock log message", None, None)
    cmdp.send_log(rec1)
    # check that we have a packet ready to be read
    assert len(mock_packet_queue_sender[send_port]) == 3
    rec2 = mock_transmitter_b.recv()
    # check that the packet is processed
    assert len(mock_packet_queue_sender[send_port]) == 0
    assert rec2.getMessage() == "mock log message"


@pytest.mark.forked
def test_stat_transmission(mock_transmitter_a, mock_transmitter_b):
    cmdp, m = mock_transmitter_a
    m1 = Metric("mock_val", "a mocked value", "Mmocs", MetricsType.LAST_VALUE, 42)
    assert not m1.sender
    cmdp.send(m1)
    # check that we have a packet ready to be read
    assert len(mock_packet_queue_sender[send_port]) == 3
    m2 = mock_transmitter_b.recv()
    # check that the packet is processed
    assert len(mock_packet_queue_sender[send_port]) == 0
    assert m2.name == "MOCK_VAL"
    assert m2.value == 42
    assert m2.sender == "mock_cmdp"
    assert m2.time


@pytest.mark.forked
def test_log_monitoring(mock_listener, mock_monitoringsender):
    m = mock_monitoringsender  # noqa
    listener, stream = mock_listener
    # ROOT logger needs to have a level set
    logger = logging.getLogger()
    logger.setLevel("DEBUG")
    # get a "remote" logger
    lr = logging.getLogger("mock_monitor")
    lr.warning("mock warning before start")
    assert len(mock_packet_queue_sender[send_port]) == 3
    listener.start()
    time.sleep(0.1)
    # processed?
    assert len(mock_packet_queue_sender[send_port]) == 0
    assert stream.handle.called
    # check arg to mock call
    assert isinstance(stream.mock_calls[0][1][0], logging.LogRecord)
    lr.info("mock info")
    time.sleep(0.1)
    assert len(mock_packet_queue_sender[send_port]) == 0
    assert len(stream.mock_calls) == 2


@pytest.mark.forked
def test_monitoring_sender_init(mock_listener, mock_monitoringsender):
    m = mock_monitoringsender
    # is our method registered?
    assert len(m._metrics_callbacks) == 1


@pytest.mark.forked
def test_monitoring_sender_loop(mock_listener, mock_monitoringsender):
    m = mock_monitoringsender
    assert send_port not in mock_packet_queue_sender
    # start metric sender thread
    m._add_com_thread()
    m._start_com_threads()
    time.sleep(0.3)
    assert b"STATS/GET_ANSWER" in mock_packet_queue_sender[send_port]
