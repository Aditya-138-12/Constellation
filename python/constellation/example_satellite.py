#!/usr/bin/env python3
"""
SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
SPDX-License-Identifier: CC-BY-4.0

This module provides the class for a Constellation Satellite.
"""

from .satellite import Satellite
import time
import logging
from .confighandler import unpack_config, ConfigError


class ExampleDevice:
    def __init__(self, config):
        self.config = config
        self.set_device_configuration()

    def set_device_configuration(self):
        try:
            self.voltage = self.config["Device"]["Voltage"]
            self.ampere = self.config["Device"]["Ampere"]
            self.sample_period = self.config["Times"]["Sample_Period"]
        except KeyError:
            raise ConfigError("Config key missing!")


class ExampleSatellite(Satellite):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.device = None

    def do_initializing(self, payload: any) -> str:
        self.config = payload
        try:
            self.device = ExampleDevice(config=unpack_config(self.config))
        except ConfigError:
            self.log.error("Configuration file incomplete!")
            raise ConfigError
        return "Initialized"

    def do_run(self, payload: any) -> str:
        while not self._state_thread_evt.is_set():
            time.sleep(self.device.sample_period)
            print(f"New sample at {self.device.voltage}")
        return "Finished acquisition."


def main(args=None):
    """Start the base Satellite server."""
    import argparse

    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--log-level", default="info")
    parser.add_argument("--cmd-port", type=int, default=23999)
    parser.add_argument("--log-port", type=int, default=5556)
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
    s = ExampleSatellite(
        args.name,
        args.group,
        args.cmd_port,
        args.hb_port,
        args.log_port,
    )
    s.run_satellite()


if __name__ == "__main__":
    main()
