/**
 * @file
 * @brief Implementation of Dictionary
 *
 * @copyright Copyright (c) 2024 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "Dictionary.hpp"

#include <msgpack.hpp>

#include <chrono>
#include <cstdint>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "constellation/core/utils/std23.hpp"

using namespace constellation::config;

void List::msgpack_pack(msgpack::packer<msgpack::sbuffer>& msgpack_packer) const {
    msgpack_packer.pack_array(this->size());

    for(auto const& val : *this) {
        msgpack_packer.pack(val);
    }
}

void List::msgpack_unpack(const msgpack::object& msgpack_object) {
    // Unpack map
    if(msgpack_object.type != msgpack::type::ARRAY) {
        throw msgpack::type_error();
    }
    const auto msgpack_array_raw = msgpack_object.via.array; // NOLINT(cppcoreguidelines-pro-type-union-access)
    const auto msgpack_array = std::span(msgpack_array_raw.ptr, msgpack_array_raw.size);

    for(const auto& msgpack_val : msgpack_array) {
        auto value = Value();
        value.msgpack_unpack(msgpack_val);

        // Insert / overwrite in the map
        this->emplace_back(std::move(value));
    }
}

void Dictionary::msgpack_pack(msgpack::packer<msgpack::sbuffer>& msgpack_packer) const {
    msgpack_packer.pack_map(this->size());

    for(auto const& [key, val] : *this) {
        msgpack_packer.pack(key);
        msgpack_packer.pack(val);
    }
}

void Dictionary::msgpack_unpack(const msgpack::object& msgpack_object) {
    // Unpack map
    if(msgpack_object.type != msgpack::type::MAP) {
        throw msgpack::type_error();
    }
    const auto msgpack_map_raw = msgpack_object.via.map; // NOLINT(cppcoreguidelines-pro-type-union-access)
    const auto msgpack_map = std::span(msgpack_map_raw.ptr, msgpack_map_raw.size);

    for(const auto& msgpack_kv : msgpack_map) {
        // Unpack key
        if(msgpack_kv.key.type != msgpack::type::STR) {
            throw msgpack::type_error();
        }
        const auto key = msgpack_kv.key.as<std::string>();

        // Unpack value
        auto value = Value();
        value.msgpack_unpack(msgpack_kv.val);

        // Insert / overwrite in the map
        this->insert_or_assign(key, std::move(value));
    }
}
