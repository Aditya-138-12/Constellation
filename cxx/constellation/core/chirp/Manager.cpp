/**
 * @file
 * @brief Implementation of the CHIRP manager
 *
 * @copyright Copyright (c) 2023 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "Manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <utility>

#include "constellation/core/logging/log.hpp"
#include "constellation/core/message/CHIRPMessage.hpp"
#include "constellation/core/message/exceptions.hpp"
#include "constellation/core/utils/casts.hpp"
#include "constellation/core/utils/std23.hpp"

using namespace constellation::chirp;
using namespace constellation::message;
using namespace constellation::utils;
using namespace std::literals::chrono_literals;

bool RegisteredService::operator<(const RegisteredService& other) const {
    // Sort first by service id
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if(std::is_lt(ord_id)) {
        return true;
    }
    if(std::is_gt(ord_id)) {
        return false;
    }
    // Then by port
    return port < other.port;
}

bool DiscoveredService::operator<(const DiscoveredService& other) const {
    // Ignore IP when sorting, we only care about the host
    auto ord_host_id = host_id <=> other.host_id;
    if(std::is_lt(ord_host_id)) {
        return true;
    }
    if(std::is_gt(ord_host_id)) {
        return false;
    }
    // Same as RegisteredService::operator<
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if(std::is_lt(ord_id)) {
        return true;
    }
    if(std::is_gt(ord_id)) {
        return false;
    }
    return port < other.port;
}

bool DiscoverCallbackEntry::operator<(const DiscoverCallbackEntry& other) const {
    // First sort after callback address NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ord_callback = reinterpret_cast<std::uintptr_t>(callback) <=> reinterpret_cast<std::uintptr_t>(other.callback);
    if(std::is_lt(ord_callback)) {
        return true;
    }
    if(std::is_gt(ord_callback)) {
        return false;
    }
    // Then after service identifier to listen to
    return std::to_underlying(service_id) < std::to_underlying(other.service_id);
}

Manager* Manager::getDefaultInstance() {
    return Manager::default_manager_instance_;
}

void Manager::setAsDefaultInstance() {
    Manager::default_manager_instance_ = this;
}

Manager::Manager(const asio::ip::address& brd_address,
                 const asio::ip::address& any_address,
                 std::string_view group_name,
                 std::string_view host_name)
    : receiver_(any_address, CHIRP_PORT), sender_(brd_address, CHIRP_PORT), group_id_(MD5Hash(group_name)),
      host_id_(MD5Hash(host_name)), logger_("CHIRP") {}

Manager::Manager(std::string_view brd_ip, std::string_view any_ip, std::string_view group_name, std::string_view host_name)
    : Manager(asio::ip::make_address(brd_ip), asio::ip::make_address(any_ip), group_name, host_name) {}

Manager::~Manager() {
    // First stop Run function
    main_loop_thread_.request_stop();
    if(main_loop_thread_.joinable()) {
        main_loop_thread_.join();
    }
    // Now unregister all services
    unregisterServices();
}

void Manager::start() {
    // jthread immediately starts on construction
    main_loop_thread_ = std::jthread(std::bind_front(&Manager::main_loop, this));
}

bool Manager::registerService(ServiceIdentifier service_id, utils::Port port) {
    const RegisteredService service {service_id, port};

    std::unique_lock registered_services_lock {registered_services_mutex_};
    const auto insert_ret = registered_services_.insert(service);
    const bool actually_inserted = insert_ret.second;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if(actually_inserted) {
        sendMessage(OFFER, service);
    }
    return actually_inserted;
}

bool Manager::unregisterService(ServiceIdentifier service_id, utils::Port port) {
    const RegisteredService service {service_id, port};

    std::unique_lock registered_services_lock {registered_services_mutex_};
    const auto erase_ret = registered_services_.erase(service);
    const bool actually_erased = erase_ret > 0;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if(actually_erased) {
        sendMessage(DEPART, service);
    }
    return actually_erased;
}

void Manager::unregisterServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    for(auto service : registered_services_) {
        sendMessage(DEPART, service);
    }
    registered_services_.clear();
}

std::set<RegisteredService> Manager::getRegisteredServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    return registered_services_;
}

bool Manager::registerDiscoverCallback(DiscoverCallback* callback, ServiceIdentifier service_id, std::any user_data) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto insert_ret = discover_callbacks_.emplace(callback, service_id, std::move(user_data));

    // Return if actually inserted
    return insert_ret.second;
}

bool Manager::unregisterDiscoverCallback(DiscoverCallback* callback, ServiceIdentifier service_id) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto erase_ret = discover_callbacks_.erase({callback, service_id, {}});

    // Return if actually erased
    return erase_ret > 0;
}

void Manager::unregisterDiscoverCallbacks() {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    discover_callbacks_.clear();
}

void Manager::forgetDiscoveredServices() {
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    discovered_services_.clear();
}

std::vector<DiscoveredService> Manager::getDiscoveredServices() {
    std::vector<DiscoveredService> ret {};
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    std::copy(discovered_services_.begin(), discovered_services_.end(), std::back_inserter(ret));
    return ret;
}

std::vector<DiscoveredService> Manager::getDiscoveredServices(ServiceIdentifier service_id) {
    std::vector<DiscoveredService> ret {};
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    for(const auto& discovered_service : discovered_services_) {
        if(discovered_service.identifier == service_id) {
            ret.push_back(discovered_service);
        }
    }
    return ret;
}

void Manager::sendRequest(ServiceIdentifier service) {
    sendMessage(REQUEST, {service, 0});
}

void Manager::sendMessage(MessageType type, RegisteredService service) {
    LOG(logger_, DEBUG) << "Sending " << to_string(type) << " for " << to_string(service.identifier) << " service on port "
                        << service.port;
    const auto asm_msg = CHIRPMessage(type, group_id_, host_id_, service.identifier, service.port).assemble();
    sender_.sendBroadcast(asm_msg);
}

void Manager::main_loop(const std::stop_token& stop_token) {
    while(!stop_token.stop_requested()) {
        try {
            const auto raw_msg_opt = receiver_.asyncRecvBroadcast(50ms);

            // Check for timeout
            if(!raw_msg_opt.has_value()) {
                continue;
            }

            const auto& raw_msg = raw_msg_opt.value();
            auto chirp_msg = CHIRPMessage::disassemble(raw_msg.content);

            LOG(logger_, TRACE) << "Received message from " << raw_msg.address.to_string()
                                << ": group = " << chirp_msg.getGroupID().to_string()
                                << ", host = " << chirp_msg.getHostID().to_string()
                                << ", type = " << to_string(chirp_msg.getType())
                                << ", service = " << to_string(chirp_msg.getServiceIdentifier())
                                << ", port = " << chirp_msg.getPort();

            if(chirp_msg.getGroupID() != group_id_) {
                // Broadcast from different group, ignore
                continue;
            }
            if(chirp_msg.getHostID() == host_id_) {
                // Broadcast from self, ignore
                continue;
            }

            const DiscoveredService discovered_service {
                raw_msg.address, chirp_msg.getHostID(), chirp_msg.getServiceIdentifier(), chirp_msg.getPort()};

            switch(chirp_msg.getType()) {
            case REQUEST: {
                auto service_id = discovered_service.identifier;
                LOG(logger_, DEBUG) << "Received REQUEST for " << to_string(service_id) << " services";
                const std::lock_guard registered_services_lock {registered_services_mutex_};
                // Replay OFFERs for registered services with same service identifier
                for(const auto& service : registered_services_) {
                    if(service.identifier == service_id) {
                        sendMessage(OFFER, service);
                    }
                }
                break;
            }
            case OFFER: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if(!discovered_services_.contains(discovered_service)) {
                    discovered_services_.insert(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();

                    LOG(logger_, DEBUG) << to_string(chirp_msg.getServiceIdentifier()) << " service at "
                                        << raw_msg.address.to_string() << ":" << chirp_msg.getPort() << " discovered";

                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for(const auto& cb_entry : discover_callbacks_) {
                        if(cb_entry.service_id == discovered_service.identifier) {
                            std::thread(cb_entry.callback, discovered_service, false, cb_entry.user_data).detach();
                        }
                    }
                }
                break;
            }
            case DEPART: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if(discovered_services_.contains(discovered_service)) {
                    discovered_services_.erase(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();

                    LOG(logger_, DEBUG) << to_string(chirp_msg.getServiceIdentifier()) << " service at "
                                        << raw_msg.address.to_string() << ":" << chirp_msg.getPort() << " departed";

                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for(const auto& cb_entry : discover_callbacks_) {
                        if(cb_entry.service_id == discovered_service.identifier) {
                            std::thread(cb_entry.callback, discovered_service, true, cb_entry.user_data).detach();
                        }
                    }
                }
                break;
            }
            default: std::unreachable();
            }
        } catch(const MessageDecodingError& error) {
            LOG(logger_, WARNING) << error.what();
            continue;
        }
    }
}
