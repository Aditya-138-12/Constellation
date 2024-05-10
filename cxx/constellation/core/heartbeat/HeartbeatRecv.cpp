/**
 * @file
 * @brief Heartbeat receiver implementation
 *
 * @copyright Copyright (c) 2024 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "HeartbeatRecv.hpp"

#include <any>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "constellation/core/chirp/Manager.hpp"
#include "constellation/core/logging/log.hpp"
#include "constellation/core/logging/Logger.hpp"
#include "constellation/core/message/CHP1Message.hpp"
#include "constellation/core/message/exceptions.hpp"
#include "constellation/core/utils/casts.hpp"
#include "constellation/core/utils/std23.hpp"
#include "constellation/core/utils/string.hpp"

using namespace constellation;
using namespace constellation::heartbeat;
using namespace constellation::log;
using namespace constellation::message;
using namespace constellation::utils;
using namespace std::literals::chrono_literals;

HeartbeatRecv::HeartbeatRecv(std::function<void(const message::CHP1Message&)> fct)
    : logger_("CHP"), message_callback_(std::move(fct)) {

    auto* chirp_manager = chirp::Manager::getDefaultInstance();
    if(chirp_manager != nullptr) {
        // Register CHIRP callback
        chirp_manager->registerDiscoverCallback(&HeartbeatRecv::callback, chirp::HEARTBEAT, this);
        // Request currently active heartbeating services
        chirp_manager->sendRequest(chirp::HEARTBEAT);
    }

    // Start the receiver thread
    receiver_thread_ = std::jthread(std::bind_front(&HeartbeatRecv::loop, this));
}

HeartbeatRecv::~HeartbeatRecv() {
    auto* chirp_manager = chirp::Manager::getDefaultInstance();
    if(chirp_manager != nullptr) {
        // Unregister CHIRP discovery callback:
        chirp_manager->unregisterDiscoverCallback(&HeartbeatRecv::callback, chirp::HEARTBEAT);
    }

    // Stop the receiver thread
    receiver_thread_.request_stop();
    cv_.notify_one();

    if(receiver_thread_.joinable()) {
        receiver_thread_.join();
    }

    // Disconnect from all remote sockets
    disconnect_all();
}

void HeartbeatRecv::connect(const chirp::DiscoveredService& service) {
    const std::lock_guard sockets_lock {sockets_mutex_};

    // Connect
    LOG(logger_, TRACE) << "Connecting to " << service.to_uri() << "...";
    try {

        zmq::socket_t socket {context_, zmq::socket_type::sub};
        socket.connect(service.to_uri());
        socket.set(zmq::sockopt::subscribe, "");

        /**
         * This lambda is passed to the ZMQ active_poller_t to be called when a socket has a incoming message pending. Since
         * this is set per-socket, we can pass a reference to the currently registered socket to the lambda and then directly
         * access the socket, read the ZMQ message and pass it to the message callback.
         */
        const zmq::active_poller_t::handler_type handler = [this, sock = zmq::socket_ref(socket)](zmq::event_flags ef) {
            // Check if flags indicate the correct ZMQ event (pollin, incoming message):
            if((ef & zmq::event_flags::pollin) != zmq::event_flags::none) {
                zmq::multipart_t zmq_msg {};
                auto received = zmq_msg.recv(sock);
                if(received) {
                    try {
                        const auto msg = CHP1Message::disassemble(zmq_msg);
                        message_callback_(msg);
                    } catch(const MessageDecodingError& error) {
                        LOG(logger_, WARNING) << error.what();
                    } catch(const IncorrectMessageType& error) {
                        LOG(logger_, WARNING) << error.what();
                    }
                }
            }
        };

        // Register the socket with the poller
        poller_.add(socket, zmq::event_flags::pollin, handler);
        sockets_.emplace(service, std::move(socket));
        LOG(logger_, DEBUG) << "Connected to " << service.to_uri();
    } catch(const zmq::error_t& e) {
        // FIXME rollback registration?
        LOG(logger_, DEBUG) << "Error when registering socket for " << service.to_uri();
    }
}

void HeartbeatRecv::disconnect_all() {
    const std::lock_guard sockets_lock {sockets_mutex_};

    // Unregister all sockets from the poller, then disconnect and close them.
    for(auto& [service, socket] : sockets_) {
        try {
            poller_.remove(zmq::socket_ref(socket));
            socket.disconnect(service.to_uri());
            socket.close();
        } catch(const zmq::error_t& e) {
            LOG(logger_, DEBUG) << "Error disconnecting socket for " << service.to_uri();
        }
    }
    sockets_.clear();
}

void HeartbeatRecv::disconnect(const chirp::DiscoveredService& service) {
    const std::lock_guard sockets_lock {sockets_mutex_};

    // Disconnect the socket
    const auto socket_it = sockets_.find(service);
    if(socket_it != sockets_.end()) {
        LOG(logger_, TRACE) << "Disconnecting from " << service.to_uri() << "...";
        try {
            // Remove from poller
            poller_.remove(zmq::socket_ref(socket_it->second));
            socket_it->second.disconnect(service.to_uri());
            socket_it->second.close();
        } catch(const zmq::error_t& e) {
            LOG(logger_, DEBUG) << "Error disconnecting socket for " << socket_it->first.to_uri();
        }

        sockets_.erase(socket_it);
        LOG(logger_, DEBUG) << "Disconnected from " << service.to_uri();
    }
}

void HeartbeatRecv::callback_impl(const chirp::DiscoveredService& service, bool depart) {
    LOG(logger_, TRACE) << "Callback for " << service.to_uri() << (depart ? ", departing" : "");

    if(depart) {
        disconnect(service);
    } else {
        connect(service);
    }

    // Ping the main thread
    cv_.notify_one();
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
void HeartbeatRecv::callback(chirp::DiscoveredService service, bool depart, std::any user_data) {
    auto* instance = std::any_cast<HeartbeatRecv*>(user_data);
    instance->callback_impl(service, depart);
}

void HeartbeatRecv::loop(const std::stop_token& stop_token) {
    while(!stop_token.stop_requested()) {
        std::unique_lock<std::mutex> lock(sockets_mutex_);
        cv_.wait(lock, [this, stop_token] { return !sockets_.empty() || stop_token.stop_requested(); });
        lock.unlock();

        // Poller crashes if called with no sockets attached:
        if(!sockets_.empty()) {
            // The poller returns immediately when a socket received something, but will time out after the set period (1s):
            poller_.wait(1000ms);
        }
    }
}
