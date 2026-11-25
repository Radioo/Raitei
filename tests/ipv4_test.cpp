#include "core/ipv4.h"

#include <catch2/catch_test_macros.hpp>

using raitei::format_address;
using raitei::is_virtual_address;
using raitei::parse_address;

TEST_CASE("only 100.64.0.0/10 counts as a virtual address") {
    CHECK(is_virtual_address(*parse_address("100.64.0.0")));
    CHECK(is_virtual_address(*parse_address("100.88.12.34")));
    CHECK(is_virtual_address(*parse_address("100.127.255.255")));
    CHECK_FALSE(is_virtual_address(*parse_address("100.63.255.255")));
    CHECK_FALSE(is_virtual_address(*parse_address("100.128.0.0")));
    CHECK_FALSE(is_virtual_address(*parse_address("10.20.30.40")));
    CHECK_FALSE(is_virtual_address(*parse_address("127.0.0.1")));
}

TEST_CASE("addresses survive a format and parse round trip") {
    for (const auto* text : {"100.64.0.1", "10.20.30.40", "0.0.0.0", "255.255.255.255"}) {
        const auto parsed = parse_address(text);
        REQUIRE(parsed);
        CHECK(format_address(*parsed) == text);
    }
}

TEST_CASE("text that is not a dotted IPv4 address does not parse") {
    for (const auto* text : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "a.b.c.d", " 1.2.3.4", "1.2.3.4 ", "::1"}) {
        CHECK_FALSE(parse_address(text));
    }
}
