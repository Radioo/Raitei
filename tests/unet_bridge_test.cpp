#include "loopback_signaling.h"

#include "core/ipv4.h"
#include "core/tunnel.h"
#include "games/ccj/unet_bridge.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

using namespace raitei;
using raitei::ccj::UnetBridge;
using raitei::testing::eventually;
using raitei::testing::LoopbackSignaling;

namespace {

constexpr WORD winsock_version = MAKEWORD(2, 2);
constexpr DWORD wait_milliseconds = 50;
constexpr DWORD receive_timeout_milliseconds = 2000;
constexpr std::size_t receive_capacity = 2048;
constexpr std::string_view greeting = "hello";
constexpr std::string_view reply = "world";
constexpr std::string_view direct = "direct";
constexpr auto unanswered_for = std::chrono::seconds{1};

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        REQUIRE(WSAStartup(winsock_version, &data) == 0);
    }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
    ~WinsockSession() {
        WSACleanup();
    }
};

ccj::Winsock real_winsock() {
    return ccj::Winsock{.sendto = &::sendto, .receive_from = &::WSARecvFrom, .completion = &::GetQueuedCompletionStatus, .close = &::closesocket};
}

struct Cabinet {
    Cabinet(LoopbackSignaling& hub, const std::string& address)
        : signaling(hub.join(address)), address(*parse_address(signaling.address())), tunnel(signaling), bridge(tunnel, real_winsock()) {
    }

    LoopbackSignaling::Endpoint& signaling;
    std::uint32_t address;
    Tunnel tunnel;
    UnetBridge bridge;
};

sockaddr_in endpoint(std::uint32_t address, std::uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = address;
    result.sin_port = port;
    return result;
}

SOCKET open_udp() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(socket != INVALID_SOCKET);
    const auto any = endpoint(INADDR_ANY, 0);
    REQUIRE(::bind(socket, reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0);
    return socket;
}

std::uint16_t port_of(SOCKET socket) {
    sockaddr_in local{};
    int length = sizeof local;
    REQUIRE(::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0);
    return local.sin_port;
}

struct Arrival {
    sockaddr_in from;
    std::string bytes;
};

class UnetSocket {
public:
    UnetSocket() : socket_(open_udp()), port_(CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket_), nullptr, 0, 0)) {
        REQUIRE(port_ != nullptr);
    }
    UnetSocket(const UnetSocket&) = delete;
    UnetSocket& operator=(const UnetSocket&) = delete;
    ~UnetSocket() {
        if (open_) {
            ::closesocket(socket_);
        }
        CloseHandle(port_);
    }

    SOCKET socket() const {
        return socket_;
    }

    std::uint16_t port() const {
        return port_of(socket_);
    }

    void post(UnetBridge& bridge) {
        overlapped_ = {};
        from_ = {};
        from_length_ = sizeof from_;
        flags_ = 0;
        buffer_ = WSABUF{static_cast<ULONG>(bytes_.size()), bytes_.data()};
        const int posted = bridge.receive_from(socket_, &buffer_, 1, nullptr, &flags_, reinterpret_cast<sockaddr*>(&from_), &from_length_, &overlapped_, nullptr);
        REQUIRE((posted == 0 || WSAGetLastError() == WSA_IO_PENDING));
    }

    std::optional<Arrival> wait(UnetBridge& bridge) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED completed = nullptr;
        if (!bridge.completion(port_, &transferred, &key, &completed, wait_milliseconds) || completed != &overlapped_) {
            return std::nullopt;
        }
        return Arrival{from_, std::string(bytes_.data(), transferred)};
    }

    void close(UnetBridge& bridge) {
        REQUIRE(bridge.close(socket_) == 0);
        open_ = false;
    }

private:
    SOCKET socket_;
    HANDLE port_;
    bool open_ = true;
    OVERLAPPED overlapped_{};
    sockaddr_in from_{};
    int from_length_ = sizeof from_;
    DWORD flags_ = 0;
    WSABUF buffer_{};
    std::array<char, receive_capacity> bytes_{};
};

int send_text(UnetBridge& bridge, SOCKET socket, std::string_view text, const sockaddr_in& to) {
    return bridge.sendto(socket, text.data(), static_cast<int>(text.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

std::optional<Arrival> exchange(Cabinet& sender, UnetSocket& from, std::string_view text, const sockaddr_in& to, Cabinet& receiver, UnetSocket& at) {
    std::optional<Arrival> arrival;
    eventually([&] {
        send_text(sender.bridge, from.socket(), text, to);
        arrival = at.wait(receiver.bridge);
        return arrival.has_value();
    });
    return arrival;
}

}

TEST_CASE("a joiner's datagram reaches the host from the joiner's virtual address and the reply comes back from the host's") {
    WinsockSession winsock;
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    UnetSocket host_socket;
    UnetSocket joiner_socket;
    host_socket.post(host.bridge);
    joiner_socket.post(joiner.bridge);
    host.bridge.start_matching();

    const auto at_host = exchange(joiner, joiner_socket, greeting, endpoint(host.address, host_socket.port()), host, host_socket);
    REQUIRE(at_host.has_value());
    CHECK(at_host->bytes == greeting);
    CHECK(at_host->from.sin_addr.s_addr == joiner.address);
    CHECK(at_host->from.sin_port == joiner_socket.port());

    host_socket.post(host.bridge);
    const auto at_joiner = exchange(host, host_socket, reply, at_host->from, joiner, joiner_socket);
    REQUIRE(at_joiner.has_value());
    CHECK(at_joiner->bytes == reply);
    CHECK(at_joiner->from.sin_addr.s_addr == host.address);
    CHECK(at_joiner->from.sin_port == host_socket.port());
}

TEST_CASE("a datagram to a real address goes to the real network untouched") {
    WinsockSession winsock;
    LoopbackSignaling hub;
    Cabinet cabinet(hub, "100.64.0.1");
    const SOCKET sender = open_udp();
    const SOCKET receiver = open_udp();
    const DWORD timeout = receive_timeout_milliseconds;
    REQUIRE(::setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == 0);

    REQUIRE(send_text(cabinet.bridge, sender, direct, endpoint(htonl(INADDR_LOOPBACK), port_of(receiver))) == static_cast<int>(direct.size()));

    std::array<char, receive_capacity> bytes{};
    sockaddr_in from{};
    int from_length = sizeof from;
    const int received = ::recvfrom(receiver, bytes.data(), static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&from), &from_length);
    CHECK(std::string_view(bytes.data(), received > 0 ? static_cast<std::size_t>(received) : 0) == direct);
    CHECK(from.sin_port == port_of(sender));
    ::closesocket(sender);
    ::closesocket(receiver);
}

TEST_CASE("after both cabinets close their UNet sockets the next match links again") {
    WinsockSession winsock;
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    {
        UnetSocket host_socket;
        UnetSocket joiner_socket;
        host_socket.post(host.bridge);
        joiner_socket.post(joiner.bridge);
        host.bridge.start_matching();
        REQUIRE(exchange(joiner, joiner_socket, greeting, endpoint(host.address, host_socket.port()), host, host_socket).has_value());
        host_socket.close(host.bridge);
        joiner_socket.close(joiner.bridge);
    }

    UnetSocket host_socket;
    UnetSocket joiner_socket;
    host_socket.post(host.bridge);
    joiner_socket.post(joiner.bridge);
    host.bridge.start_matching();
    const auto at_host = exchange(joiner, joiner_socket, greeting, endpoint(host.address, host_socket.port()), host, host_socket);
    REQUIRE(at_host.has_value());
    CHECK(at_host->from.sin_addr.s_addr == joiner.address);
    CHECK(at_host->from.sin_port == joiner_socket.port());
}

TEST_CASE("once both cabinets closed their UNet sockets a joiner cannot reach a host that has not started matching again") {
    WinsockSession winsock;
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    {
        UnetSocket host_socket;
        UnetSocket joiner_socket;
        host_socket.post(host.bridge);
        joiner_socket.post(joiner.bridge);
        host.bridge.start_matching();
        REQUIRE(exchange(joiner, joiner_socket, greeting, endpoint(host.address, host_socket.port()), host, host_socket).has_value());
        host_socket.close(host.bridge);
        joiner_socket.close(joiner.bridge);
    }

    UnetSocket host_socket;
    UnetSocket joiner_socket;
    host_socket.post(host.bridge);
    joiner_socket.post(joiner.bridge);
    CHECK_FALSE(eventually(
        [&] {
            send_text(joiner.bridge, joiner_socket.socket(), greeting, endpoint(host.address, host_socket.port()));
            return host_socket.wait(host.bridge).has_value();
        },
        unanswered_for));
}

TEST_CASE("a joiner that dialled before the host was matching reaches it once the host starts") {
    WinsockSession winsock;
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    UnetSocket host_socket;
    UnetSocket joiner_socket;
    host_socket.post(host.bridge);
    joiner_socket.post(joiner.bridge);
    REQUIRE_FALSE(eventually(
        [&] {
            send_text(joiner.bridge, joiner_socket.socket(), greeting, endpoint(host.address, host_socket.port()));
            return host_socket.wait(host.bridge).has_value();
        },
        unanswered_for));

    host.bridge.start_matching();
    const auto at_host = exchange(joiner, joiner_socket, greeting, endpoint(host.address, host_socket.port()), host, host_socket);
    REQUIRE(at_host.has_value());
    CHECK(at_host->from.sin_addr.s_addr == joiner.address);
}
