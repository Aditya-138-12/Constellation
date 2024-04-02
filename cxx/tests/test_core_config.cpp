/**
 * @copyright Copyright (c) 2024 DESY and the Constellation authors.
 * This software is distributed under the terms of the EUPL-1.2 License, copied verbatim in the file "LICENSE.md".
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <chrono>
#include <ctime>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <msgpack.hpp>

#include "constellation/core/config/Configuration.hpp"

using namespace Catch::Matchers;
using namespace constellation::config;

// NOLINTBEGIN(cert-err58-cpp,misc-use-anonymous-namespace)

TEST_CASE("Set & Get Values", "[core][core::config]") {
    Configuration config;

    config.set("bool", true);

    config.set("int64", std::int64_t(63));
    config.set("size", size_t(1));
    config.set("uint64", std::uint64_t(64));
    config.set("uint8", std::uint8_t(8));

    config.set("double", double(1.3));
    config.set("float", float(3.14));

    config.set("string", std::string("a"));

    enum MyEnum {
        ONE,
        TWO,
    };
    config.set("myenum", MyEnum::ONE);

    // Check that keys are unused
    REQUIRE(config.size() == config.getUnusedKeys().size());

    // Read values back
    REQUIRE(config.get<bool>("bool") == true);

    REQUIRE(config.get<std::int64_t>("int64") == 63);
    REQUIRE(config.get<size_t>("size") == 1);
    REQUIRE(config.get<std::uint64_t>("uint64") == 64);
    REQUIRE(config.get<std::uint8_t>("uint8") == 8);

    REQUIRE(config.get<double>("double") == 1.3);
    REQUIRE(config.get<float>("float") == 3.14F);

    REQUIRE(config.get<std::string>("string") == "a");

    REQUIRE(config.get<MyEnum>("myenum") == MyEnum::ONE);

    // Check that all keys have been marked as used
    REQUIRE(config.getUnusedKeys().empty());
}

TEST_CASE("Set Value & Mark Used", "[core][core::config]") {
    Configuration config;

    config.set("myval", 3.14, true);

    // Check that the key is marked as used
    REQUIRE(config.getUnusedKeys().empty());
    REQUIRE(config.get<double>("myval") == 3.14);
}

TEST_CASE("Set Default Value", "[core][core::config]") {
    Configuration config;

    // Check that a default does not overwrite existing values
    config.set("myval", true);
    config.setDefault("myval", false);
    REQUIRE(config.get<bool>("myval") == true);

    // Check that a default is set when the value does not exist
    config.setDefault("mydefault", false);
    REQUIRE(config.get<bool>("mydefault") == false);
}

TEST_CASE("Invalid Key Access", "[core][core::config]") {
    Configuration config;

    // Check for invalid key to be detected
    REQUIRE_THROWS_AS(config.get<bool>("invalidkey"), MissingKeyError);
    REQUIRE_THROWS_MATCHES(config.get<bool>("invalidkey"), MissingKeyError, Message("Key 'invalidkey' does not exist"));

    // Check for invalid type conversion
    config.set("key", true);
    REQUIRE_THROWS_AS(config.get<double>("key"), InvalidTypeError);
    REQUIRE_THROWS_MATCHES(config.get<double>("key"),
                           InvalidTypeError,
                           Message("Could not convert value of type 'bool' to type 'double' for key 'key'"));

    // Check for invalid enum value conversion:
    enum MyEnum {
        ONE,
        TWO,
    };
    config.set("myenum", "THREE");
    REQUIRE_THROWS_AS(config.get<MyEnum>("myenum"), InvalidValueError);
    REQUIRE_THROWS_MATCHES(config.get<MyEnum>("myenum"),
                           InvalidValueError,
                           Message("Value THREE of key 'myenum' is not valid: possible values are one, two"));
}
// NOLINTEND(cert-err58-cpp,misc-use-anonymous-namespace)
