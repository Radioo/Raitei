#pragma once

#include "core/tunnel.h"

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace raitei::ccj {

struct Winsock {
    decltype(&::sendto) sendto;
    decltype(&::WSARecvFrom) receive_from;
    decltype(&::GetQueuedCompletionStatus) completion;
    decltype(&::closesocket) close;
};

struct Endpoint {
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    auto operator<=>(const Endpoint&) const = default;
};

class UnetBridge {
public:
    UnetBridge(Tunnel& tunnel, const Winsock& real);
    UnetBridge(const UnetBridge&) = delete;
    UnetBridge& operator=(const UnetBridge&) = delete;
    ~UnetBridge();

    void start_matching();
    int sendto(SOCKET socket, const char* buffer, int length, int flags, const sockaddr* to, int to_length);
    int receive_from(SOCKET socket, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from, LPINT from_length,
        LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine);
    BOOL completion(HANDLE port, LPDWORD transferred, PULONG_PTR key, LPOVERLAPPED* overlapped, DWORD milliseconds);
    int close(SOCKET socket);

private:
    struct PendingReceive {
        SOCKET socket;
        sockaddr* from;
        LPINT from_length;
    };

    void deliver(const Datagram& datagram);
    std::optional<SOCKET> proxy_for(const Endpoint& remote, const Endpoint& target);
    void take_accepted();
    bool keeps_existing(int existing, int accepted) const;
    void link_to(std::uint32_t remote);
    void reset(std::uint64_t generation);

    Tunnel& tunnel_;
    Winsock real_;
    std::mutex mutex_;
    std::map<SOCKET, Endpoint> unet_sockets_;
    std::map<LPOVERLAPPED, PendingReceive> pending_;
    std::map<std::uint32_t, int> links_;
    std::map<int, std::chrono::steady_clock::time_point> dialled_;
    int next_handle_ = 0;
    std::uint64_t generation_ = 0;
    std::map<Endpoint, SOCKET> proxies_;
    std::map<Endpoint, Endpoint> proxied_remotes_;
    std::uint64_t dropped_ = 0;
};

}
