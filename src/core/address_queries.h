#pragma once

#include "signaling.h"

#include <cstdint>
#include <optional>

namespace raitei {

using NetQuery = std::int32_t (*)(std::uint32_t command, void* out, std::int32_t size);
using StatusQuery = std::int32_t (*)(std::uint32_t command, const char* path, void* out, std::int32_t size);

struct TunnelAddress {
    bool configured = false;
    std::optional<std::uint32_t> address;
};

inline constexpr std::int32_t query_failed = -1;

TunnelAddress tunnel_address(const SignalingClient& signaling);
bool is_global_address_query(std::uint32_t command, const char* path);
std::int32_t answer_net_query(NetQuery real, const TunnelAddress& tunnel, std::uint32_t command, void* out, std::int32_t size);
std::int32_t answer_status_query(StatusQuery real, const TunnelAddress& tunnel, std::uint32_t command, const char* path, void* out, std::int32_t size);

}
