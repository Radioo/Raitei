#include "frames.h"

#include <spdlog/spdlog.h>

#include <bit>
#include <cstring>

namespace raitei {

namespace {

constexpr std::size_t frame_type_offset = 8;

std::uint16_t frame_type(const std::uint8_t* frame) {
    std::uint16_t type = 0;
    std::memcpy(&type, frame + frame_type_offset, sizeof type);
    return type;
}

void write_session_key(std::uint8_t* frame, std::uint64_t key) {
    std::memcpy(frame, &key, sizeof key);
}

void point_local_endpoint_at_global(std::uint8_t* frame, std::size_t size) {
    const auto type = frame_type(frame);
    if ((type == joiner_intro_type || type == peer_info_type) && size >= intro_frame_size) {
        std::memcpy(frame + intro_local_endpoint, frame + intro_global_endpoint, intro_endpoint_size);
    }
}

}

std::uint64_t session_key(std::uint32_t network_order) {
    return std::byteswap(network_order);
}

int deliver(const Received& received, std::uint8_t* buffer, int capacity, std::uint64_t key) {
    switch (received.status) {
    case Received::Status::empty:
        return nothing_received;
    case Received::Status::closed:
        return disconnected;
    case Received::Status::frame:
        break;
    }

    const auto size = received.bytes.size();
    if (size < frame_header_size || capacity < 0 || size > static_cast<std::size_t>(capacity)) {
        spdlog::warn("dropping a {} byte frame", size);
        return nothing_received;
    }
    std::memcpy(buffer, received.bytes.data(), size);
    write_session_key(buffer, key);
    point_local_endpoint_at_global(buffer, size);
    return static_cast<int>(size);
}

}
