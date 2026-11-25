#pragma once

#include <cstdint>
#include <optional>

namespace raitei::iidx {

using NetQuery = std::int32_t (*)(std::uint32_t command, void* out, std::int32_t size);
using StatusQuery = std::int32_t (*)(std::uint32_t command, const char* path, void* out, std::int32_t size);

struct TunnelAddress {
    bool configured = false;
    std::optional<std::uint32_t> address;
};

inline constexpr std::int32_t query_failed = -1;

std::int32_t answer_net_query(NetQuery real, const TunnelAddress& tunnel, std::uint32_t command, void* out, std::int32_t size);
std::int32_t answer_status_query(StatusQuery real, const TunnelAddress& tunnel, std::uint32_t command, const char* path, void* out, std::int32_t size);

}
