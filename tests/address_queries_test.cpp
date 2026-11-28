#include "core/import_table.h"
#include "core/ipv4.h"
#include "core/address_queries.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>

using namespace raitei;

namespace {

constexpr std::int32_t real_answer = 55;
constexpr std::uint32_t own_address = 0;
constexpr std::uint32_t own_mask = 3;
constexpr std::uint32_t dns_servers = 2;
constexpr std::uint32_t facility_ip4 = 9;
constexpr std::uint32_t full_mask = 0xFFFFFFFFu;
constexpr auto global_address = "portfw/globalip";
constexpr auto global_port = "portfw/globalport";

struct RealQueries {
    int net_calls = 0;
    int status_calls = 0;
    std::uint32_t last_command = 0;
    std::string last_path;
};

RealQueries real_queries;

std::int32_t real_net(std::uint32_t command, void*, std::int32_t) {
    ++real_queries.net_calls;
    real_queries.last_command = command;
    return real_answer;
}

std::int32_t real_status(std::uint32_t command, const char* path, void*, std::int32_t) {
    ++real_queries.status_calls;
    real_queries.last_command = command;
    real_queries.last_path = path == nullptr ? "" : path;
    return real_answer;
}

std::uint32_t read_address(const std::array<std::uint8_t, 8>& out) {
    std::uint32_t address = 0;
    std::memcpy(&address, out.data(), sizeof address);
    return address;
}

const TunnelAddress unconfigured{};
const TunnelAddress unregistered{.configured = true};

TunnelAddress registered() {
    return TunnelAddress{.configured = true, .address = parse_address("100.64.0.7")};
}

}

TEST_CASE("without signaling every address query goes to avs") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, unconfigured, own_address, out.data(), 8) == real_answer);
    CHECK(answer_net_query(&real_net, unconfigured, own_mask, out.data(), 8) == real_answer);
    CHECK(answer_status_query(&real_status, unconfigured, facility_ip4, global_address, out.data(), 8) == real_answer);
    CHECK(real_queries.net_calls == 2);
    CHECK(real_queries.status_calls == 1);
}

TEST_CASE("a registered cabinet's own address is its virtual address") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, registered(), own_address, out.data(), 8) == 4);
    CHECK(read_address(out) == *parse_address("100.64.0.7"));
    CHECK(real_queries.net_calls == 0);
}

TEST_CASE("the netmask is a single address whether or not signaling registered yet") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, registered(), own_mask, out.data(), 8) == 4);
    CHECK(read_address(out) == full_mask);
    out.fill(0);
    CHECK(answer_net_query(&real_net, unregistered, own_mask, out.data(), 8) == 4);
    CHECK(read_address(out) == full_mask);
    CHECK(real_queries.net_calls == 0);
}

TEST_CASE("before registration the own address comes from avs") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, unregistered, own_address, out.data(), 8) == real_answer);
    CHECK(real_queries.net_calls == 1);
    CHECK(real_queries.last_command == own_address);
}

TEST_CASE("other network queries go to avs") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, registered(), dns_servers, out.data(), 8) == real_answer);
    CHECK(real_queries.last_command == dns_servers);
}

TEST_CASE("an answer that does not fit fails") {
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_net_query(&real_net, registered(), own_address, out.data(), 3) == query_failed);
    CHECK(answer_net_query(&real_net, registered(), own_mask, nullptr, 8) == query_failed);
    CHECK(answer_status_query(&real_status, registered(), facility_ip4, global_address, out.data(), 2) == query_failed);
}

TEST_CASE("the global address is the virtual address once registered and fails before") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_status_query(&real_status, unregistered, facility_ip4, global_address, out.data(), 8) == query_failed);
    CHECK(answer_status_query(&real_status, registered(), facility_ip4, global_address, out.data(), 8) == 4);
    CHECK(read_address(out) == *parse_address("100.64.0.7"));
    CHECK(real_queries.status_calls == 0);
}

TEST_CASE("other facility values go to avs") {
    real_queries = {};
    std::array<std::uint8_t, 8> out{};

    CHECK(answer_status_query(&real_status, registered(), facility_ip4, global_port, out.data(), 8) == real_answer);
    CHECK(real_queries.last_path == global_port);
    CHECK(answer_status_query(&real_status, registered(), facility_ip4, nullptr, out.data(), 8) == real_answer);
    CHECK(answer_status_query(&real_status, registered(), own_mask, global_address, out.data(), 8) == real_answer);
    CHECK(real_queries.status_calls == 3);
}

TEST_CASE("an import the module does not have is not found") {
    CHECK(find_ordinal_import(GetModuleHandleA(nullptr), "avs2-core.dll", 122) == nullptr);
}

TEST_CASE("writing an import slot replaces the pointer it holds") {
    int original = 0;
    int detour = 0;
    void* slot = &original;

    REQUIRE(write_import(&slot, &detour));
    CHECK(slot == &detour);
}
