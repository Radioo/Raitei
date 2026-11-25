#include "address_queries.h"

#include "core/ipv4.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string_view>

namespace raitei::iidx {

namespace {

constexpr std::uint32_t own_address_query = 0;
constexpr std::uint32_t own_mask_query = 3;
constexpr std::uint32_t facility_ip4_query = 9;
constexpr std::string_view global_address_path = "portfw/globalip";
constexpr std::uint32_t single_address_mask = 0xFFFFFFFFu;

std::int32_t write_address(std::uint32_t address, void* out, std::int32_t size) {
    if (out == nullptr || size < static_cast<std::int32_t>(sizeof address)) {
        return query_failed;
    }
    std::memcpy(out, &address, sizeof address);
    return static_cast<std::int32_t>(sizeof address);
}

}

std::int32_t answer_net_query(NetQuery real, const TunnelAddress& tunnel, std::uint32_t command, void* out, std::int32_t size) {
    if (!tunnel.configured) {
        return real(command, out, size);
    }
    if (command == own_mask_query) {
        spdlog::info("the game's subnet is its own address alone, so it dials every peer by its global address");
        return write_address(single_address_mask, out, size);
    }
    if (command != own_address_query) {
        return real(command, out, size);
    }
    if (!tunnel.address) {
        spdlog::error("the game read its local address before signaling registered one, so it reports its LAN address to peers");
        return real(command, out, size);
    }
    spdlog::info("the game uses {} as its local address", format_address(*tunnel.address));
    return write_address(*tunnel.address, out, size);
}

std::int32_t answer_status_query(StatusQuery real, const TunnelAddress& tunnel, std::uint32_t command, const char* path, void* out, std::int32_t size) {
    if (command != facility_ip4_query || path == nullptr || global_address_path != path || !tunnel.configured) {
        return real(command, path, out, size);
    }
    if (!tunnel.address) {
        spdlog::info("the game asked for its global address before signaling registered one, it asks again when matching starts");
        return query_failed;
    }
    spdlog::info("the game advertises {} as its global address", format_address(*tunnel.address));
    return write_address(*tunnel.address, out, size);
}

}
