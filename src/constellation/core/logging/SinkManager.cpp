/**
 * @file
 * @brief Implementation of the Log Sink Manager
 *
 * @copyright Copyright (c) 2023 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "SinkManager.hpp"

#include <spdlog/async.h>
#include <spdlog/spdlog.h>

#include <string_view>

using namespace constellation::log;
using namespace std::literals::string_view_literals;

SinkManager& SinkManager::getInstance() {
    static SinkManager instance {};
    return instance;
}

void SinkManager::setGlobalConsoleLevel(Level level) const {
    console_sink_->set_level(to_spdlog_level(level));
}

SinkManager::SinkManager() {
    // Init thread pool with 1k queue size on 1 thread
    spdlog::init_thread_pool(1000, 1);

    console_sink_ = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    // Set formatting
    console_sink_->set_pattern("|%Y-%m-%d %H:%M:%S.%e| %^%8l%$ [%n] %v");

    // Set colors of console sink
    console_sink_->set_color(to_spdlog_level(CRITICAL), "\x1B[31;1m"sv); // Bold red
    console_sink_->set_color(to_spdlog_level(STATUS), "\x1B[32;1m"sv);   // Bold green
    console_sink_->set_color(to_spdlog_level(WARNING), "\x1B[33;1m"sv);  // Bold yellow
    console_sink_->set_color(to_spdlog_level(INFO), "\x1B[36;1m"sv);     // Bold cyan
    console_sink_->set_color(to_spdlog_level(DEBUG), "\x1B[36m"sv);      // Cyan
    console_sink_->set_color(to_spdlog_level(TRACE), "\x1B[90m"sv);      // Grey

    cmdp1_sink_ = std::make_shared<CMDP1Sink>();
    cmdp1_sink_->set_level(spdlog::level::trace);
}

std::shared_ptr<spdlog::async_logger> SinkManager::createLogger(std::string topic) {
    auto logger = std::make_shared<spdlog::async_logger>(std::move(topic),
                                                         spdlog::sinks_init_list({console_sink_, cmdp1_sink_}),
                                                         spdlog::thread_pool(),
                                                         spdlog::async_overflow_policy::overrun_oldest);
    return logger;
}
