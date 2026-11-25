#include "games/sdvx/frames.h"
#include "core/ipv4.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

using namespace raitei;

namespace {

constexpr std::uint64_t receiver_key = 0x000000000a141e28ULL;
constexpr int game_capacity = 528;

std::vector<std::uint8_t> numbered_frame(std::size_t size, std::uint16_t type) {
    std::vector<std::uint8_t> frame(size);
    std::iota(frame.begin(), frame.end(), std::uint8_t{1});
    std::memcpy(frame.data() + 8, &type, sizeof type);
    return frame;
}

Received arrived(std::vector<std::uint8_t> bytes) {
    return Received{Received::Status::frame, std::move(bytes)};
}

std::uint64_t key_in(const std::vector<std::uint8_t>& buffer) {
    std::uint64_t key = 0;
    std::memcpy(&key, buffer.data(), sizeof key);
    return key;
}

}

TEST_CASE("the session key is the big-endian value of the address") {
    CHECK(session_key(*parse_address("10.20.30.40")) == receiver_key);
    CHECK(session_key(*parse_address("100.88.12.34")) == 0x0000000064580c22ULL);
}

TEST_CASE("nothing to read and a closed link map to the game's return codes") {
    std::vector<std::uint8_t> buffer(game_capacity);
    CHECK(deliver(Received{Received::Status::empty, {}}, buffer.data(), game_capacity, receiver_key) == nothing_received);
    CHECK(deliver(Received{Received::Status::closed, {}}, buffer.data(), game_capacity, receiver_key) == disconnected);
}

TEST_CASE("a frame gets the receiver's session key and keeps the rest") {
    const auto frame = numbered_frame(64, 7);
    std::vector<std::uint8_t> buffer(game_capacity);

    REQUIRE(deliver(arrived(frame), buffer.data(), game_capacity, receiver_key) == 64);
    CHECK(key_in(buffer) == receiver_key);
    CHECK(std::equal(frame.begin() + 8, frame.end(), buffer.begin() + 8));
}

TEST_CASE("frames the game cannot hold are dropped instead of overflowing its buffer") {
    std::vector<std::uint8_t> buffer(game_capacity, 0xEE);

    CHECK(deliver(arrived(numbered_frame(game_capacity + 1, 7)), buffer.data(), game_capacity, receiver_key) == nothing_received);
    CHECK(deliver(arrived(std::vector<std::uint8_t>(frame_header_size - 1, 1)), buffer.data(), game_capacity, receiver_key) == nothing_received);
    CHECK(std::all_of(buffer.begin(), buffer.end(), [](std::uint8_t byte) { return byte == 0xEE; }));
}

TEST_CASE("joiner intro and peer info frames name the sender's global endpoint twice") {
    for (const auto type : {joiner_intro_type, peer_info_type}) {
        const auto frame = numbered_frame(intro_frame_size, type);
        std::vector<std::uint8_t> buffer(game_capacity);

        REQUIRE(deliver(arrived(frame), buffer.data(), game_capacity, receiver_key) == static_cast<int>(intro_frame_size));
        CHECK(std::equal(frame.begin() + intro_global_endpoint, frame.begin() + intro_global_endpoint + intro_endpoint_size,
            buffer.begin() + intro_local_endpoint));
        CHECK(std::equal(frame.begin() + intro_global_endpoint, frame.begin() + intro_global_endpoint + intro_endpoint_size,
            buffer.begin() + intro_global_endpoint));
    }
}

TEST_CASE("other frames and short intro frames keep their endpoints") {
    for (const auto& frame : {numbered_frame(intro_frame_size, 5), numbered_frame(intro_frame_size - 1, joiner_intro_type)}) {
        std::vector<std::uint8_t> buffer(game_capacity);

        REQUIRE(deliver(arrived(frame), buffer.data(), game_capacity, receiver_key) == static_cast<int>(frame.size()));
        CHECK(std::equal(frame.begin() + 8, frame.end(), buffer.begin() + 8));
    }
}
