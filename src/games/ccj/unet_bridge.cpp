#include "unet_bridge.h"

#include "core/ipv4.h"

#include <ws2tcpip.h>
#include <mstcpip.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <utility>

namespace raitei::ccj {

namespace {

constexpr std::size_t source_port_at = 0;
constexpr std::size_t destination_port_at = sizeof(std::uint16_t);
constexpr std::size_t header_size = 2 * sizeof(std::uint16_t);
constexpr std::size_t mapped_address_at = 12;
constexpr std::size_t max_proxies = 64;
constexpr std::uint32_t any_address = 0;
constexpr std::uint16_t any_port = 0;
constexpr auto unanswered_link_deadline = std::chrono::seconds{5};

class LastError {
public:
    LastError() : error_(GetLastError()) {
    }
    LastError(const LastError&) = delete;
    LastError& operator=(const LastError&) = delete;
    ~LastError() {
        SetLastError(error_);
    }

private:
    DWORD error_;
};

std::optional<Endpoint> read_endpoint(const sockaddr* address, int length) {
    if (address == nullptr) {
        return std::nullopt;
    }
    if (address->sa_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        return Endpoint{v4->sin_addr.s_addr, v4->sin_port};
    }
    if (address->sa_family != AF_INET6 || length < static_cast<int>(sizeof(sockaddr_in6))) {
        return std::nullopt;
    }
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
    if (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr)) {
        return Endpoint{any_address, v6->sin6_port};
    }
    if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
        return std::nullopt;
    }
    std::uint32_t mapped = 0;
    std::memcpy(&mapped, v6->sin6_addr.s6_addr + mapped_address_at, sizeof mapped);
    return Endpoint{mapped, v6->sin6_port};
}

void write_endpoint(sockaddr* address, int length, const Endpoint& endpoint) {
    if (address->sa_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in))) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(address);
        v4->sin_addr.s_addr = endpoint.address;
        v4->sin_port = endpoint.port;
        return;
    }
    if (address->sa_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6))) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(address);
        IN6_SET_ADDR_V4MAPPED(&v6->sin6_addr, reinterpret_cast<const IN_ADDR*>(&endpoint.address));
        v6->sin6_port = endpoint.port;
    }
}

sockaddr_in to_sockaddr(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = endpoint.address;
    address.sin_port = endpoint.port;
    return address;
}

std::optional<Endpoint> local_endpoint(SOCKET socket) {
    sockaddr_storage storage{};
    int length = sizeof storage;
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return std::nullopt;
    }
    return read_endpoint(reinterpret_cast<const sockaddr*>(&storage), length);
}

std::uint16_t read_port(const std::vector<std::uint8_t>& frame, std::size_t at) {
    std::uint16_t port = 0;
    std::memcpy(&port, frame.data() + at, sizeof port);
    return port;
}

}

UnetBridge::UnetBridge(Tunnel& tunnel, const Winsock& real) : tunnel_(tunnel), real_(real) {
    tunnel_.on_datagram([this](const Datagram& datagram) { deliver(datagram); });
}

UnetBridge::~UnetBridge() {
    tunnel_.on_datagram(nullptr);
    for (const auto& [remote, proxy] : proxies_) {
        ::closesocket(proxy);
    }
}

void UnetBridge::start_matching() {
    {
        std::scoped_lock lock(mutex_);
        ++generation_;
    }
    tunnel_.start_accepting();
    spdlog::info("matching started, peers can link to this cabinet");
}

int UnetBridge::sendto(SOCKET socket, const char* buffer, int length, int flags, const sockaddr* to, int to_length) {
    const auto destination = read_endpoint(to, to_length);
    if (!destination || !is_virtual_address(destination->address) || buffer == nullptr || length < 0) {
        return real_.sendto(socket, buffer, length, flags, to, to_length);
    }
    const auto local = local_endpoint(socket);
    if (!local) {
        return real_.sendto(socket, buffer, length, flags, to, to_length);
    }

    std::vector<std::uint8_t> frame(header_size + static_cast<std::size_t>(length));
    std::memcpy(frame.data() + source_port_at, &local->port, sizeof local->port);
    std::memcpy(frame.data() + destination_port_at, &destination->port, sizeof destination->port);
    std::memcpy(frame.data() + header_size, buffer, static_cast<std::size_t>(length));

    take_accepted();
    link_to(destination->address);
    tunnel_.send_datagram(destination->address, frame);
    WSASetLastError(0);
    return length;
}

int UnetBridge::receive_from(SOCKET socket, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from, LPINT from_length,
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
    const bool tracked = overlapped != nullptr && from != nullptr && from_length != nullptr;
    if (tracked) {
        const auto local = local_endpoint(socket);
        std::scoped_lock lock(mutex_);
        pending_[overlapped] = PendingReceive{socket, from, from_length};
        if (local && unet_sockets_.insert_or_assign(socket, *local).second) {
            ++generation_;
            spdlog::info("UNet receives on {}:{}", format_address(local->address), ntohs(local->port));
        }
    }
    const int result = real_.receive_from(socket, buffers, count, received, flags, from, from_length, overlapped, routine);
    if (tracked && result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        const LastError keep;
        std::scoped_lock lock(mutex_);
        if (const auto found = pending_.find(overlapped); found != pending_.end() && found->second.socket == socket) {
            pending_.erase(found);
        }
    }
    return result;
}

BOOL UnetBridge::completion(HANDLE port, LPDWORD transferred, PULONG_PTR key, LPOVERLAPPED* overlapped, DWORD milliseconds) {
    const BOOL result = real_.completion(port, transferred, key, overlapped, milliseconds);
    if (overlapped == nullptr || *overlapped == nullptr) {
        return result;
    }

    const LastError keep;
    std::scoped_lock lock(mutex_);
    const auto found = pending_.find(*overlapped);
    if (found == pending_.end()) {
        return result;
    }
    const auto pending = found->second;
    pending_.erase(found);
    if (result == FALSE || transferred == nullptr || *transferred == 0) {
        return result;
    }

    const auto source = read_endpoint(pending.from, *pending.from_length);
    if (!source) {
        return result;
    }
    const auto remote = proxied_remotes_.find(*source);
    if (remote != proxied_remotes_.end()) {
        write_endpoint(pending.from, *pending.from_length, remote->second);
    }
    return result;
}

int UnetBridge::close(SOCKET socket) {
    std::optional<std::uint64_t> last_at;
    {
        std::scoped_lock lock(mutex_);
        std::erase_if(pending_, [socket](const auto& entry) { return entry.second.socket == socket; });
        if (unet_sockets_.erase(socket) != 0 && unet_sockets_.empty()) {
            last_at = generation_;
        }
    }
    const int result = real_.close(socket);
    if (last_at) {
        const LastError keep;
        reset(*last_at);
    }
    return result;
}

void UnetBridge::deliver(const Datagram& datagram) {
    if (datagram.bytes.size() < header_size) {
        return;
    }
    const Endpoint remote{datagram.from, read_port(datagram.bytes, source_port_at)};
    const auto destination_port = read_port(datagram.bytes, destination_port_at);

    std::scoped_lock lock(mutex_);
    const auto unet = std::ranges::find_if(unet_sockets_, [destination_port](const auto& entry) { return entry.second.port == destination_port; });
    if (unet == unet_sockets_.end()) {
        if (std::has_single_bit(++dropped_)) {
            spdlog::warn("datagram from {} for port {} dropped: UNet does not receive there ({} dropped so far)", format_address(remote.address),
                ntohs(destination_port), dropped_);
        }
        return;
    }
    const Endpoint target{unet->second.address == any_address ? htonl(INADDR_LOOPBACK) : unet->second.address, destination_port};
    const auto proxy = proxy_for(remote, target);
    if (!proxy) {
        return;
    }
    const auto to = to_sockaddr(target);
    ::sendto(*proxy, reinterpret_cast<const char*>(datagram.bytes.data() + header_size), static_cast<int>(datagram.bytes.size() - header_size), 0,
        reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

std::optional<SOCKET> UnetBridge::proxy_for(const Endpoint& remote, const Endpoint& target) {
    if (const auto found = proxies_.find(remote); found != proxies_.end()) {
        return found->second;
    }
    if (proxies_.size() >= max_proxies) {
        spdlog::warn("datagram from {}:{} dropped: {} remote endpoints already reach the game", format_address(remote.address), ntohs(remote.port),
            max_proxies);
        return std::nullopt;
    }

    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        spdlog::warn("could not open a socket for {}: {}", format_address(remote.address), WSAGetLastError());
        return std::nullopt;
    }
    const auto bind_to = to_sockaddr(Endpoint{target.address, any_port});
    const auto local = ::bind(socket, reinterpret_cast<const sockaddr*>(&bind_to), sizeof bind_to) == 0 ? local_endpoint(socket) : std::nullopt;
    if (!local) {
        spdlog::warn("could not bind a socket for {}: {}", format_address(remote.address), WSAGetLastError());
        ::closesocket(socket);
        return std::nullopt;
    }

    proxies_.emplace(remote, socket);
    proxied_remotes_.emplace(*local, remote);
    spdlog::info("{}:{} reaches UNet from {}:{}", format_address(remote.address), ntohs(remote.port), format_address(local->address), ntohs(local->port));
    return socket;
}

void UnetBridge::take_accepted() {
    while (true) {
        std::optional<int> closing;
        {
            std::scoped_lock lock(mutex_);
            const int handle = next_handle_;
            if (!tunnel_.has_ready() || !tunnel_.accept_into(handle)) {
                return;
            }
            ++next_handle_;
            const auto remote = tunnel_.remote(handle);
            if (!remote) {
                continue;
            }
            const auto [entry, inserted] = links_.try_emplace(*remote, handle);
            if (!inserted) {
                const int existing = entry->second;
                if (keeps_existing(existing, handle)) {
                    closing = handle;
                } else {
                    closing = existing;
                    entry->second = handle;
                    dialled_.erase(existing);
                }
            }
        }
        if (closing) {
            tunnel_.close(*closing);
        }
    }
}

bool UnetBridge::keeps_existing(int existing, int accepted) const {
    if (tunnel_.state(existing) == LinkState::closed) {
        return false;
    }
    const auto existing_link = tunnel_.link(existing);
    const auto accepted_link = tunnel_.link(accepted);
    return existing_link && accepted_link && *existing_link < *accepted_link;
}

void UnetBridge::link_to(std::uint32_t remote) {
    std::optional<int> stale;
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = links_.find(remote); found != links_.end()) {
            const auto state = tunnel_.state(found->second);
            const auto dialled = dialled_.find(found->second);
            const bool unanswered = state == LinkState::connecting && dialled != dialled_.end()
                && std::chrono::steady_clock::now() - dialled->second >= unanswered_link_deadline;
            if (state != LinkState::closed && !unanswered) {
                return;
            }
            stale = found->second;
            dialled_.erase(found->second);
        }
        const int handle = next_handle_++;
        links_[remote] = handle;
        dialled_[handle] = std::chrono::steady_clock::now();
        tunnel_.connect(handle, remote);
    }
    if (stale) {
        tunnel_.close(*stale);
    }
    spdlog::info("UNet sends to {}, linking to it", format_address(remote));
}

void UnetBridge::reset(std::uint64_t generation) {
    std::vector<int> handles;
    std::vector<SOCKET> sockets;
    {
        std::scoped_lock lock(mutex_);
        if (generation != generation_ || !unet_sockets_.empty()) {
            return;
        }
        std::ranges::transform(links_, std::back_inserter(handles), [](const auto& entry) { return entry.second; });
        std::ranges::transform(proxies_, std::back_inserter(sockets), [](const auto& entry) { return entry.second; });
        links_.clear();
        dialled_.clear();
        proxies_.clear();
        proxied_remotes_.clear();
    }
    spdlog::info("UNet closed its last socket, closing {} links", handles.size());
    tunnel_.stop_accepting();
    for (const auto handle : handles) {
        tunnel_.close(handle);
    }
    for (const auto socket : sockets) {
        ::closesocket(socket);
    }
}

}
