/**
 * @file
 * @brief Implementation of base satellite
 *
 * @copyright Copyright (c) 2024 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "BaseSatellite.hpp"

#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <magic_enum.hpp>
#include <msgpack.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "constellation/build.hpp"
#include "constellation/core/chirp/CHIRP_definitions.hpp"
#include "constellation/core/chirp/Manager.hpp"
#include "constellation/core/config/Configuration.hpp"
#include "constellation/core/config/Dictionary.hpp"
#include "constellation/core/heartbeat/HeartbeatManager.hpp"
#include "constellation/core/logging/log.hpp"
#include "constellation/core/message/CSCP1Message.hpp"
#include "constellation/core/message/exceptions.hpp"
#include "constellation/core/message/PayloadBuffer.hpp"
#include "constellation/core/message/satellite_definitions.hpp"
#include "constellation/core/utils/exceptions.hpp"
#include "constellation/core/utils/networking.hpp"
#include "constellation/core/utils/std_future.hpp"
#include "constellation/core/utils/string.hpp"
#include "constellation/satellite/exceptions.hpp"
#include "constellation/satellite/fsm_definitions.hpp"

using namespace constellation;
using namespace constellation::config;
using namespace constellation::heartbeat;
using namespace constellation::message;
using namespace constellation::satellite;
using namespace constellation::utils;
using namespace std::literals::chrono_literals;

BaseSatellite::BaseSatellite(std::string_view type, std::string_view name)
    : logger_("SATELLITE"), rep_socket_(context_, zmq::socket_type::rep), port_(bind_ephemeral_port(rep_socket_)),
      satellite_type_(type), satellite_name_(name), fsm_(this), cscp_logger_("CSCP"),
      heartbeat_manager_(getCanonicalName(), [&]() { return fsm_.getState(); }) {

    // Check name
    if(!is_valid_name(to_string(name))) {
        throw RuntimeError("Satellite name is invalid");
    }

    // Set receive timeout for CSCP socket
    rep_socket_.set(zmq::sockopt::rcvtimeo, static_cast<int>(std::chrono::milliseconds(100).count()));

    // Announce service via CHIRP
    auto* chirp_manager = chirp::Manager::getDefaultInstance();
    if(chirp_manager != nullptr) {
        chirp_manager->registerService(chirp::CONTROL, port_);
    } else {
        LOG(cscp_logger_, WARNING)
            << "Failed to advertise command receiver on the network, satellite might not be discovered";
    }
    LOG(cscp_logger_, INFO) << "Starting to listen to commands on port " << port_;

    // Start receiving CSCP commands
    cscp_thread_ = std::jthread(std::bind_front(&BaseSatellite::cscp_loop, this));

    // Start sending heartbeats
    heartbeat_manager_.setInterruptCallback([&]() { fsm_.requestInterrupt(); });
    fsm_.registerStateCallback([&](State) { heartbeat_manager_.sendExtrasystole(); });
}

BaseSatellite::~BaseSatellite() {
    cscp_thread_.request_stop();
    join();
}

std::string BaseSatellite::getCanonicalName() const {
    return to_string(satellite_type_) + "." + to_string(satellite_name_);
}

void BaseSatellite::join() {
    if(cscp_thread_.joinable()) {
        cscp_thread_.join();
    }
}

void BaseSatellite::terminate() {
    // Request stop on the CSCP thread
    cscp_thread_.request_stop();

    // We cannot join the CSCP thread here since this method might be called from there and would result in a race condition

    // Tell the FSM to interrupt as soon as possible, which will go to SAFE in case of ORBIT or RUN state:
    fsm_.requestInterrupt();
}

std::optional<CSCP1Message> BaseSatellite::get_next_command() {
    // Receive next message
    zmq::multipart_t recv_msg {};
    auto received = recv_msg.recv(rep_socket_);

    // Return if timeout
    if(!received) {
        return std::nullopt;
    }

    // Try to disamble message
    auto message = CSCP1Message::disassemble(recv_msg);

    LOG(cscp_logger_, DEBUG) << "Received CSCP message of type " << to_string(message.getVerb().first) << " with verb \""
                             << message.getVerb().second << "\"" << (message.hasPayload() ? " and a payload"sv : ""sv)
                             << " from " << message.getHeader().getSender();

    return message;
}

void BaseSatellite::send_reply(std::pair<CSCP1Message::Type, std::string> reply_verb, message::PayloadBuffer payload) {
    auto msg = CSCP1Message({getCanonicalName()}, std::move(reply_verb));
    msg.addPayload(std::move(payload));
    msg.assemble().send(rep_socket_);
}

std::optional<std::pair<std::pair<message::CSCP1Message::Type, std::string>, message::PayloadBuffer>>
BaseSatellite::handle_standard_command(std::string_view command) {
    std::pair<message::CSCP1Message::Type, std::string> return_verb {};
    message::PayloadBuffer return_payload {};

    auto command_enum = magic_enum::enum_cast<StandardCommand>(command, magic_enum::case_insensitive);
    if(!command_enum.has_value()) {
        return std::nullopt;
    }

    using enum StandardCommand;
    switch(command_enum.value()) {
    case get_name: {
        return_verb = {CSCP1Message::Type::SUCCESS, getCanonicalName()};
        break;
    }
    case get_version: {
        return_verb = {CSCP1Message::Type::SUCCESS, CNSTLN_VERSION};
        break;
    }
    case get_commands: {
        return_verb = {CSCP1Message::Type::SUCCESS, "Commands attached in payload"};
        auto command_dict = Dictionary();
        // FSM commands
        command_dict["initialize"] = "Initialize satellite (payload: config as flat MessagePack dict with strings as keys)";
        command_dict["launch"] = "Launch satellite";
        command_dict["land"] = "Land satellite";
        if(support_reconfigure_) {
            command_dict["reconfigure"] =
                "Reconfigure satellite (payload: partial config as flat MessagePack dict with strings as keys)";
        }
        command_dict["start"] = "Start new run (payload: run number as MessagePack integer)";
        command_dict["stop"] = "Stop run";
        command_dict["shutdown"] = "Shutdown satellite";
        // Get commands
        command_dict["get_name"] = "Get canonical name of satellite";
        command_dict["get_version"] = "Get Constellation version of satellite";
        command_dict["get_commands"] =
            "Get commands supported by satellite (returned in payload as flat MessagePack dict with strings as keys)";
        command_dict["get_state"] = "Get state of satellite";
        command_dict["get_status"] = "Get status of satellite";
        command_dict["get_config"] =
            "Get config of satellite (returned in payload as flat MessagePack dict with strings as keys)";

        // Append user commands
        const auto user_commands = user_commands_.describeCommands();
        for(const auto& cmd : user_commands) {
            command_dict.emplace(cmd.first, cmd.second);
        }

        // Pack dict
        return_payload = command_dict.assemble();
        break;
    }
    case get_state: {
        return_verb = {CSCP1Message::Type::SUCCESS, to_string(fsm_.getState())};
        break;
    }
    case get_status: {
        return_verb = {CSCP1Message::Type::SUCCESS, status_};
        break;
    }
    case get_config: {
        return_verb = {CSCP1Message::Type::SUCCESS, "Configuration attached in payload"};
        return_payload = config_.getDictionary(Configuration::Group::ALL, Configuration::Usage::USED).assemble();
        break;
    }
    case get_run_id: {
        return_verb = {CSCP1Message::Type::SUCCESS, run_identifier_};
        break;
    }
    case shutdown: {
        if(is_shutdown_allowed(fsm_.getState())) {
            return_verb = {CSCP1Message::Type::SUCCESS, "Shutting down satellite"};
            terminate();
        } else {
            return_verb = {CSCP1Message::Type::INVALID,
                           "Satellite cannot be shut down from current state " + to_string(fsm_.getState())};
        }
        break;
    }
    default: std::unreachable();
    }

    return std::make_pair(return_verb, std::move(return_payload));
}

std::optional<std::pair<std::pair<message::CSCP1Message::Type, std::string>, message::PayloadBuffer>>
BaseSatellite::handle_user_command(std::string_view command, const message::PayloadBuffer& payload) {
    LOG(cscp_logger_, DEBUG) << "Attempting to handle command \"" << command << "\" as user command";

    std::pair<message::CSCP1Message::Type, std::string> return_verb {};
    message::PayloadBuffer return_payload {};

    config::List args {};
    try {
        if(!payload.empty()) {
            args = config::List::disassemble(payload);
        }

        auto retval = user_commands_.call(fsm_.getState(), std::string(command), args);
        LOG(cscp_logger_, DEBUG) << "User command \"" << command << "\" succeeded, packing return value.";

        // Return the call value as payload only if it is not std::monostate
        if(!std::holds_alternative<std::monostate>(retval)) {
            msgpack::sbuffer sbuf {};
            msgpack::pack(sbuf, retval);
            return_payload = {std::move(sbuf)};
        }
        return_verb = {CSCP1Message::Type::SUCCESS, {}};
    } catch(const std::bad_cast&) {
        // Issue with obtaining parameters from payload
        return_verb = {CSCP1Message::Type::INCOMPLETE, "Could not convert command payload to argument list"};
    } catch(const UnknownUserCommand&) {
        return std::nullopt;
    } catch(const InvalidUserCommand& error) {
        // Command cannot be called in current state
        return_verb = {CSCP1Message::Type::INVALID, error.what()};
    } catch(const UserCommandError& error) {
        // Any other issue with executing the user command (missing arguments, wrong arguments, ...)
        return_verb = {CSCP1Message::Type::INCOMPLETE, error.what()};
    } catch(const std::exception& error) {
        LOG(cscp_logger_, DEBUG) << "Caught exception while calling user command \"" << command << "\": " << error.what();
        return std::nullopt;
    } catch(...) {
        LOG(cscp_logger_, DEBUG) << "Caught unknown exception while calling user command \"" << command << "\"";
        return std::nullopt;
    }

    return std::make_pair(return_verb, std::move(return_payload));
}

void BaseSatellite::cscp_loop(const std::stop_token& stop_token) {
    while(!stop_token.stop_requested()) {
        try {
            // Receive next command
            auto message_opt = get_next_command();

            // Timeout, continue
            if(!message_opt.has_value()) {
                continue;
            }
            const auto& message = message_opt.value();

            // Ensure we have a REQUEST message
            if(message.getVerb().first != CSCP1Message::Type::REQUEST) {
                LOG(cscp_logger_, WARNING) << "Received message via CSCP that is not REQUEST type - ignoring";
                send_reply({CSCP1Message::Type::ERROR, "Can only handle CSCP messages with REQUEST type"});
                continue;
            }

            // Transform command to lower-case
            const std::string command_string = transform(message.getVerb().second, ::tolower);

            // Try to decode as transition
            auto transition_command = magic_enum::enum_cast<TransitionCommand>(command_string, magic_enum::case_insensitive);
            if(transition_command.has_value()) {
                send_reply(fsm_.reactCommand(transition_command.value(), message.getPayload()));
                continue;
            }

            // Try to decode as other builtin (non-transition) commands
            auto standard_command_reply = handle_standard_command(command_string);
            if(standard_command_reply.has_value()) {
                send_reply(standard_command_reply.value().first, std::move(standard_command_reply.value().second));
                continue;
            }

            // Handle user-registered commands:
            auto user_command_reply = handle_user_command(command_string, message.getPayload());
            if(user_command_reply.has_value()) {
                send_reply(user_command_reply.value().first, std::move(user_command_reply.value().second));
                continue;
            }

            // Command is not known
            std::string unknown_command_reply = "Command \"";
            unknown_command_reply += command_string;
            unknown_command_reply += "\" is not known";
            LOG(cscp_logger_, WARNING) << "Received unknown command \"" << command_string << "\" - ignoring";
            send_reply({CSCP1Message::Type::UNKNOWN, std::move(unknown_command_reply)});

        } catch(const zmq::error_t& error) {
            LOG(cscp_logger_, CRITICAL) << "ZeroMQ error while trying to receive a message: " << error.what();
            LOG(cscp_logger_, CRITICAL) << "Stopping command receiver loop, no further commands can be received";
            break;
        } catch(const MessageDecodingError& error) {
            LOG(cscp_logger_, WARNING) << error.what();
            send_reply({CSCP1Message::Type::ERROR, error.what()});
        }
    }
}

void BaseSatellite::store_config(config::Configuration&& config) {
    using enum config::Configuration::Group;
    using enum config::Configuration::Usage;

    // Check for unused KVPs
    const auto unused_kvps = config.getDictionary(ALL, UNUSED);
    if(!unused_kvps.empty()) {
        LOG(logger_, WARNING) << unused_kvps.size() << " keys of the configuration were not used: "
                              << range_to_string(std::views::keys(unused_kvps));
        // Only store used keys
        config_ = {config.getDictionary(ALL, USED), true};
    } else {
        // Move configuration
        config_ = std::move(config);
    }

    // Log config
    LOG(logger_, INFO) << "Configuration: " << config_.size(USER) << " settings" << config_.getDictionary(USER).to_string();
    LOG(logger_, DEBUG) << "Internal configuration: " << config_.size(INTERNAL) << " settings"
                        << config_.getDictionary(INTERNAL).to_string();
}

void BaseSatellite::update_config(const config::Configuration& partial_config) {
    using enum config::Configuration::Group;
    using enum config::Configuration::Usage;

    // Check for unused KVPs
    const auto unused_kvps = partial_config.getDictionary(ALL, UNUSED);
    if(!unused_kvps.empty()) {
        LOG(logger_, WARNING) << unused_kvps.size() << " keys of the configuration were not used: "
                              << range_to_string(std::views::keys(unused_kvps));
    }

    // Update configuration (only updates used values of partial config)
    config_.update(partial_config);

    // Log config
    LOG(logger_, INFO) << "Configuration: " << config_.size(USER) << " settings" << config_.getDictionary(USER).to_string();
    LOG(logger_, DEBUG) << "Internal configuration: " << config_.size(INTERNAL) << " settings"
                        << config_.getDictionary(INTERNAL).to_string();
}

void BaseSatellite::initializing_wrapper(config::Configuration&& config) {
    initializing(config);

    // Store config after initializing
    store_config(std::move(config));
}

void BaseSatellite::launching_wrapper() {
    launching();
}

void BaseSatellite::landing_wrapper() {
    landing();
}

void BaseSatellite::reconfiguring_wrapper(const config::Configuration& partial_config) {
    reconfiguring(partial_config);

    // Update stored config after reconfigure
    update_config(partial_config);
}

void BaseSatellite::starting_wrapper(std::string run_identifier) {
    starting(run_identifier);

    // Store run identifier
    run_identifier_ = std::move(run_identifier);
}

void BaseSatellite::stopping_wrapper() {
    stopping();
}

void BaseSatellite::running_wrapper(const std::stop_token& stop_token) {
    running(stop_token);
}

void BaseSatellite::interrupting_wrapper(State previous_state) {
    interrupting(previous_state);
}

void BaseSatellite::failure_wrapper(State previous_state) {
    failure(previous_state);
}
