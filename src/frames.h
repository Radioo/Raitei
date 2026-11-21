#pragma once

#include "tunnel.h"

#include <cstddef>
#include <cstdint>

namespace raitei {

constexpr int nothing_received = 0;
constexpr int disconnected = -2;
constexpr std::size_t frame_header_size = 16;
constexpr std::uint16_t joiner_intro_type = 3;
constexpr std::uint16_t peer_info_type = 4;
constexpr std::size_t intro_global_endpoint = 256;
constexpr std::size_t intro_local_endpoint = 264;
constexpr std::size_t intro_endpoint_size = 8;
constexpr std::size_t intro_frame_size = 272;

std::uint64_t session_key(std::uint32_t network_order);
int deliver(const Received& received, std::uint8_t* buffer, int capacity, std::uint64_t key);

}
