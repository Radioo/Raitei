#include "ess_hooks.h"

#include "frames.h"

#include "core/ipv4.h"

#include <windows.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>

namespace raitei {

namespace {

constexpr int client_socket = -1;
constexpr int sent = 0;
constexpr int send_failed = -1;
constexpr int accepted = 1;
constexpr int not_accepted = 0;
constexpr int listening = 0;
constexpr int opened = 0;
constexpr int requested = 0;
constexpr int connect_done = 0;
constexpr int connect_pending = 2;
constexpr int no_connect_in_progress = 1;
constexpr int connect_succeeded = 0;
constexpr int connect_failed = -1;
constexpr int property_ok = 0;

constexpr std::size_t property_nic_ip = 0;
constexpr std::size_t property_global_ip = 8;
constexpr std::size_t property_global_port = 12;

struct Hooks {
    safetyhook::InlineHook property_get;
    safetyhook::InlineHook listen;
    safetyhook::InlineHook accept;
    safetyhook::InlineHook connect_request;
    safetyhook::InlineHook connect_wait;
    safetyhook::InlineHook tcp_sendto;
    safetyhook::InlineHook tcp_recvfrom;
    safetyhook::InlineHook close_dstsocket;
    safetyhook::InlineHook close_dstsocket_all;
    safetyhook::InlineHook client_disconnect;
    safetyhook::InlineHook udp_open;
    safetyhook::InlineHook udp_sendto;
    safetyhook::InlineHook udp_recvfrom;
};

Hooks hooks;
Tunnel* tunnel = nullptr;
const SignalingClient* signaling = nullptr;
std::atomic<std::uint64_t> host_session_key{0};
std::atomic<std::uint64_t> client_session_key{0};
std::atomic<bool> real_listening{false};
std::atomic<bool> connect_reported{false};
std::atomic<std::uint64_t> last_udp_key{0};
std::mutex udp_destinations_mutex;
std::set<std::uint32_t> udp_destinations;

int property_get(std::uint8_t* out) {
    const int result = hooks.property_get.call<int>(out);
    if (result != property_ok || out == nullptr) {
        return result;
    }

    std::uint32_t nic_ip = 0;
    std::memcpy(&nic_ip, out + property_nic_ip, sizeof nic_ip);
    host_session_key = session_key(nic_ip);

    const auto self = signaling->registration();
    if (!self) {
        return result;
    }
    const auto address = parse_address(self->address);
    if (!address) {
        return result;
    }
    const std::uint32_t port = self->port;
    std::memcpy(out + property_global_ip, &*address, sizeof *address);
    std::memcpy(out + property_global_port, &port, sizeof port);
    return result;
}

int tcp_server_listen() {
    real_listening = hooks.listen.call<int>() == listening;
    tunnel->start_accepting();
    return listening;
}

int tcp_server_accept(int socket, int second, int third) {
    if (tunnel->accept_into(socket)) {
        return accepted;
    }
    return real_listening ? hooks.accept.call<int>(socket, second, third) : not_accepted;
}

int connect_request(std::uint32_t ip, std::uint16_t port) {
    if (!is_virtual_address(ip)) {
        return hooks.connect_request.call<int>(ip, port);
    }
    client_session_key = session_key(ip);
    connect_reported = false;
    tunnel->connect(client_socket, ip);
    return requested;
}

int connect_wait(int* result) {
    if (!tunnel->owns(client_socket)) {
        return hooks.connect_wait.call<int>(result);
    }
    if (connect_reported) {
        return no_connect_in_progress;
    }
    switch (tunnel->state(client_socket)) {
    case LinkState::connecting:
        return connect_pending;
    case LinkState::open:
        *result = connect_succeeded;
        break;
    case LinkState::closed:
        *result = connect_failed;
        break;
    }
    connect_reported = true;
    return connect_done;
}

int tcp_sendto(int socket, const std::uint8_t* frame, int length) {
    if (tunnel->owns(socket)) {
        return tunnel->send(socket, std::span(frame, static_cast<std::size_t>(length))) ? sent : send_failed;
    }
    return hooks.tcp_sendto.call<int>(socket, frame, length);
}

int tcp_recvfrom(int socket, std::uint8_t* buffer, int capacity, int fourth, int fifth) {
    if (tunnel->owns(socket)) {
        const auto key = socket == client_socket ? client_session_key.load() : host_session_key.load();
        return deliver(tunnel->receive(socket), buffer, capacity, key);
    }
    return hooks.tcp_recvfrom.call<int>(socket, buffer, capacity, fourth, fifth);
}

int close_dstsocket(int socket) {
    if (tunnel->owns(socket)) {
        tunnel->close(socket);
        return 0;
    }
    return hooks.close_dstsocket.call<int>(socket);
}

int close_dstsocket_all() {
    tunnel->stop_accepting();
    real_listening = false;
    return hooks.close_dstsocket_all.call<int>();
}

int client_disconnect() {
    if (tunnel->owns(client_socket)) {
        tunnel->close(client_socket);
        return 0;
    }
    return hooks.client_disconnect.call<int>();
}

int udp_open() {
    hooks.udp_open.call<int>();
    return opened;
}

int udp_sendto(const std::uint8_t* datagram, int length, std::uint32_t ip, std::uint16_t port) {
    const bool virtual_destination = is_virtual_address(ip);
    {
        std::scoped_lock lock(udp_destinations_mutex);
        if (udp_destinations.insert(ip).second) {
            spdlog::info("first udp datagram to {}:{} goes {}", format_address(ip), port, virtual_destination ? "through the tunnel" : "to the real network");
        }
    }
    if (!virtual_destination) {
        return hooks.udp_sendto.call<int>(datagram, length, ip, port);
    }
    if (length >= static_cast<int>(sizeof(std::uint64_t))) {
        std::uint64_t key = 0;
        std::memcpy(&key, datagram, sizeof key);
        if (last_udp_key.exchange(key) != key) {
            spdlog::info("own udp session key is now {:016x}; keys written into received frames: host {:016x}, client {:016x}",
                key, host_session_key.load(), client_session_key.load());
        }
    }
    return tunnel->send_datagram(ip, std::span(datagram, static_cast<std::size_t>(length))) ? sent : send_failed;
}

int udp_recvfrom(std::uint8_t* buffer, int capacity, std::uint32_t* ip, std::uint16_t* port, int fifth, int sixth) {
    const auto datagram = tunnel->receive_datagram();
    if (!datagram) {
        return hooks.udp_recvfrom.call<int>(buffer, capacity, ip, port, fifth, sixth);
    }

    const auto key = tunnel->remote(client_socket) == datagram->from ? client_session_key.load() : host_session_key.load();
    *ip = datagram->from;
    *port = 0;
    return deliver(Received{Received::Status::frame, datagram->bytes}, buffer, capacity, key);
}

bool hook(HMODULE ess, const char* name, safetyhook::InlineHook& slot, void* detour) {
    auto* target = reinterpret_cast<void*>(GetProcAddress(ess, name));
    if (target == nullptr) {
        spdlog::error("{} not found in ess.dll", name);
        return false;
    }
    auto created = safetyhook::InlineHook::create(target, detour);
    if (!created) {
        spdlog::error("hooking {} failed", name);
        return false;
    }
    slot = std::move(*created);
    return true;
}

}

bool install_ess_hooks(HMODULE ess, Tunnel& target_tunnel, SignalingClient& target_signaling) {
    tunnel = &target_tunnel;
    signaling = &target_signaling;

    const std::array results{
        hook(ess, "ess_net_property_get", hooks.property_get, reinterpret_cast<void*>(&property_get)),
        hook(ess, "ess_net_tcp_server_listen", hooks.listen, reinterpret_cast<void*>(&tcp_server_listen)),
        hook(ess, "ess_net_tcp_server_accept", hooks.accept, reinterpret_cast<void*>(&tcp_server_accept)),
        hook(ess, "ess_net_tcp_client_connect_request", hooks.connect_request, reinterpret_cast<void*>(&connect_request)),
        hook(ess, "ess_net_tcp_client_connect_wait", hooks.connect_wait, reinterpret_cast<void*>(&connect_wait)),
        hook(ess, "ess_net_tcp_sendto", hooks.tcp_sendto, reinterpret_cast<void*>(&tcp_sendto)),
        hook(ess, "ess_net_tcp_recvfrom", hooks.tcp_recvfrom, reinterpret_cast<void*>(&tcp_recvfrom)),
        hook(ess, "ess_net_tcp_server_close_dstsocket", hooks.close_dstsocket, reinterpret_cast<void*>(&close_dstsocket)),
        hook(ess, "ess_net_tcp_server_close_dstsocket_all", hooks.close_dstsocket_all, reinterpret_cast<void*>(&close_dstsocket_all)),
        hook(ess, "ess_net_tcp_client_disconnect", hooks.client_disconnect, reinterpret_cast<void*>(&client_disconnect)),
        hook(ess, "ess_net_udp_open", hooks.udp_open, reinterpret_cast<void*>(&udp_open)),
        hook(ess, "ess_net_udp_sendto", hooks.udp_sendto, reinterpret_cast<void*>(&udp_sendto)),
        hook(ess, "ess_net_udp_recvfrom", hooks.udp_recvfrom, reinterpret_cast<void*>(&udp_recvfrom)),
    };
    return std::ranges::all_of(results, [](bool installed) { return installed; });
}

}
