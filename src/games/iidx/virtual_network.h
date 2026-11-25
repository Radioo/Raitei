#pragma once

#include "core/tunnel.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace raitei::iidx {

struct PollEntry {
    std::int32_t descriptor;
    std::uint16_t events;
    std::uint16_t revents;
    std::int32_t result;
};

struct AvsNet {
    std::int32_t (*socket)(std::int32_t type);
    std::int32_t (*bind)(std::int32_t descriptor, std::uint32_t address, std::uint16_t port);
    std::int32_t (*listen)(std::int32_t descriptor, std::int32_t backlog);
    std::int32_t (*accept)(std::int32_t descriptor, std::uint32_t* address, std::uint16_t* port);
    std::int32_t (*connect)(std::int32_t descriptor, std::uint32_t address, std::uint16_t port);
    std::int32_t (*poll)(PollEntry* entries, std::uint32_t count, std::int32_t timeout);
    std::int32_t (*send)(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length);
    std::int32_t (*recv)(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity);
    std::int32_t (*sendto)(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length, std::uint32_t address, std::uint16_t port);
    std::int32_t (*recvfrom)(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity, std::uint32_t* address, std::uint16_t* port);
    std::int32_t (*shutdown)(std::int32_t descriptor, std::int32_t how);
    std::int32_t (*close)(std::int32_t descriptor);
};

inline constexpr std::int32_t stream_socket = 0;
inline constexpr std::int32_t would_block = static_cast<std::int32_t>(0x80081000u);
inline constexpr std::int32_t in_progress = static_cast<std::int32_t>(0x80080073u);
inline constexpr std::int32_t not_connected = static_cast<std::int32_t>(0x8008006Bu);
inline constexpr std::int32_t connection_refused = static_cast<std::int32_t>(0x8008006Fu);
inline constexpr std::int32_t peer_closed = 0;
inline constexpr std::int32_t bound = 0;
inline constexpr std::uint16_t poll_writable = 4;
inline constexpr std::int32_t shutdown_reading = 0;

class VirtualNetwork {
public:
    VirtualNetwork(Tunnel& tunnel, const AvsNet& real);

    std::int32_t bind(std::int32_t descriptor, std::uint32_t address, std::uint16_t port);
    std::int32_t listen(std::int32_t descriptor, std::int32_t backlog);
    std::int32_t accept(std::int32_t descriptor, std::uint32_t* address, std::uint16_t* port);
    std::int32_t connect(std::int32_t descriptor, std::uint32_t address, std::uint16_t port);
    std::int32_t poll(PollEntry* entries, std::uint32_t count, std::int32_t timeout);
    std::int32_t send(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length);
    std::int32_t recv(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity);
    std::int32_t sendto(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length, std::uint32_t address, std::uint16_t port);
    std::int32_t recvfrom(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity, std::uint32_t* address, std::uint16_t* port);
    std::int32_t shutdown(std::int32_t descriptor, std::int32_t how);
    std::int32_t close(std::int32_t descriptor);

private:
    std::uint16_t bound_port(std::int32_t descriptor) const;
    bool is_listener(std::int32_t descriptor) const;
    bool is_tunnel_only(std::int32_t descriptor) const;
    void note_outside_tunnel(std::string_view what, std::uint32_t address, std::uint16_t port);
    Received::Status gather(std::int32_t descriptor, std::vector<std::uint8_t>& pending, std::size_t room);

    Tunnel& tunnel_;
    AvsNet real_;
    mutable std::mutex mutex_;
    std::optional<std::int32_t> listener_;
    std::map<std::int32_t, std::uint16_t> bound_ports_;
    std::map<std::int32_t, std::vector<std::uint8_t>> pending_;
    std::set<std::int32_t> finished_;
    std::set<std::int32_t> reading_closed_;
    std::set<std::int32_t> tunnel_only_;
    std::set<std::pair<std::uint32_t, std::uint16_t>> outside_tunnel_;
};

}
