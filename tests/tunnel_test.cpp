#include "ipv4.h"
#include "loopback_signaling.h"
#include "tunnel.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace raitei;
using raitei::testing::eventually;
using raitei::testing::LoopbackSignaling;

namespace {

constexpr std::size_t frame_size = 48;
constexpr auto unanswered_wait = std::chrono::seconds{2};
constexpr auto prompt_link = std::chrono::seconds{5};

struct Cabinet {
    Cabinet(LoopbackSignaling& hub, const std::string& address, std::vector<IceServer> servers = {})
        : signaling(hub.join(address, std::move(servers))), address(*parse_address(signaling.address())), tunnel(signaling) {
    }

    LoopbackSignaling::Endpoint& signaling;
    std::uint32_t address;
    Tunnel tunnel;
};

std::vector<std::uint8_t> frame(std::uint8_t fill) {
    return std::vector<std::uint8_t>(frame_size, fill);
}

std::optional<std::vector<std::uint8_t>> next_frame(const std::function<Received()>& receive) {
    std::optional<std::vector<std::uint8_t>> result;
    eventually([&] {
        auto received = receive();
        if (received.status == Received::Status::frame) {
            result = std::move(received.bytes);
            return true;
        }
        return false;
    });
    return result;
}

bool closes(const std::function<Received()>& receive) {
    return eventually([&] { return receive().status == Received::Status::closed; });
}

void link(Cabinet& host, Cabinet& joiner, int socket) {
    host.tunnel.start_accepting();
    joiner.tunnel.connect(host.address);
    REQUIRE(eventually([&] { return joiner.tunnel.client_state() == LinkState::open; }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(socket); }));
}

}

TEST_CASE("a joiner reaches a host and frames flow both ways") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    CHECK(host.tunnel.owns_socket(0));
    CHECK(joiner.tunnel.client_remote() == host.address);

    REQUIRE(joiner.tunnel.send_on_client(frame(1)));
    CHECK(next_frame([&] { return host.tunnel.receive_on_socket(0); }) == frame(1));

    REQUIRE(host.tunnel.send_on_socket(0, frame(2)));
    CHECK(next_frame([&] { return joiner.tunnel.receive_on_client(); }) == frame(2));
}

TEST_CASE("datagrams travel both ways and name their sender") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    REQUIRE(eventually([&] { return joiner.tunnel.send_datagram(host.address, frame(3)); }));
    std::optional<Datagram> at_host;
    REQUIRE(eventually([&] { return (at_host = host.tunnel.receive_datagram()).has_value(); }));
    CHECK(at_host->from == joiner.address);
    CHECK(at_host->bytes == frame(3));

    REQUIRE(host.tunnel.send_datagram(joiner.address, frame(4)));
    std::optional<Datagram> at_joiner;
    REQUIRE(eventually([&] { return (at_joiner = joiner.tunnel.receive_datagram()).has_value(); }));
    CHECK(at_joiner->from == host.address);
    CHECK(at_joiner->bytes == frame(4));
}

TEST_CASE("a host that keeps sending while the joiner disconnects never throws") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    std::atomic<bool> threw = false;
    std::jthread sender([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                host.tunnel.send_on_socket(0, frame(5));
                host.tunnel.send_datagram(joiner.address, frame(5));
            } catch (...) {
                threw = true;
            }
        }
    });

    joiner.tunnel.disconnect_client();
    CHECK(closes([&] { return host.tunnel.receive_on_socket(0); }));
    sender.request_stop();
    sender.join();

    CHECK_FALSE(threw);
    CHECK_FALSE(host.tunnel.send_on_socket(0, frame(6)));
}

TEST_CASE("a joiner that keeps sending while the host closes its socket never throws") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    std::atomic<bool> threw = false;
    std::jthread sender([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                joiner.tunnel.send_on_client(frame(7));
                joiner.tunnel.send_datagram(host.address, frame(7));
            } catch (...) {
                threw = true;
            }
        }
    });

    host.tunnel.close_socket(0);
    CHECK(closes([&] { return joiner.tunnel.receive_on_client(); }));
    sender.request_stop();
    sender.join();

    CHECK_FALSE(threw);
    CHECK_FALSE(host.tunnel.owns_socket(0));
}

TEST_CASE("stop accepting closes every joiner's link") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet first(hub, "100.64.0.2");
    Cabinet second(hub, "100.64.0.3");
    link(host, first, 0);
    link(host, second, 1);

    host.tunnel.stop_accepting();

    CHECK(closes([&] { return first.tunnel.receive_on_client(); }));
    CHECK(closes([&] { return second.tunnel.receive_on_client(); }));
    CHECK_FALSE(host.tunnel.owns_socket(0));
    CHECK_FALSE(host.tunnel.owns_socket(1));
}

TEST_CASE("an offer that arrives while the host is not accepting is ignored") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");

    joiner.tunnel.connect(host.address);

    CHECK_FALSE(eventually([&] { return joiner.tunnel.client_state() == LinkState::open; }, unanswered_wait));
    CHECK_FALSE(host.tunnel.accept_into(0));
}

TEST_CASE("a joiner can link to the same host again after disconnecting") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    joiner.tunnel.disconnect_client();
    REQUIRE(closes([&] { return host.tunnel.receive_on_socket(0); }));
    host.tunnel.close_socket(0);

    link(host, joiner, 1);
    REQUIRE(joiner.tunnel.send_on_client(frame(8)));
    CHECK(next_frame([&] { return host.tunnel.receive_on_socket(1); }) == frame(8));
}

TEST_CASE("three joiners each get their own socket") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    std::vector<std::unique_ptr<Cabinet>> joiners;
    for (const auto* address : {"100.64.0.2", "100.64.0.3", "100.64.0.4"}) {
        joiners.push_back(std::make_unique<Cabinet>(hub, address));
    }
    for (int socket = 0; socket < static_cast<int>(joiners.size()); ++socket) {
        link(host, *joiners[socket], socket);
    }

    std::set<std::uint8_t> fills_at_host;
    for (std::uint8_t index = 0; index < joiners.size(); ++index) {
        REQUIRE(joiners[index]->tunnel.send_on_client(frame(static_cast<std::uint8_t>(10 + index))));
    }
    for (int socket = 0; socket < static_cast<int>(joiners.size()); ++socket) {
        const auto received = next_frame([&] { return host.tunnel.receive_on_socket(socket); });
        REQUIRE(received);
        fills_at_host.insert(received->front());
    }
    CHECK(fills_at_host == std::set<std::uint8_t>{10, 11, 12});

    for (int socket = 0; socket < static_cast<int>(joiners.size()); ++socket) {
        REQUIRE(host.tunnel.send_on_socket(socket, frame(static_cast<std::uint8_t>(20 + socket))));
    }
    std::set<std::uint8_t> fills_at_joiners;
    for (const auto& joiner : joiners) {
        const auto received = next_frame([&] { return joiner->tunnel.receive_on_client(); });
        REQUIRE(received);
        fills_at_joiners.insert(received->front());
    }
    CHECK(fills_at_joiners == std::set<std::uint8_t>{20, 21, 22});
}

TEST_CASE("malformed signaling does not stop a host from accepting real joiners") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    host.tunnel.start_accepting();

    hub.inject("100.64.0.1", Message{.type = MessageType::offer, .from = "100.64.0.9", .sdp = "not a session description"});
    hub.inject("100.64.0.1", Message{.type = MessageType::offer, .from = "not an address", .sdp = "v=0"});
    hub.inject("100.64.0.1", Message{.type = MessageType::answer, .from = "100.64.0.9", .sdp = "v=0"});

    link(host, joiner, 0);
    REQUIRE(joiner.tunnel.send_on_client(frame(9)));
    CHECK(next_frame([&] { return host.tunnel.receive_on_socket(0); }) == frame(9));
}

TEST_CASE("a malformed answer from the dialled host leaves the joiner connecting") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");

    joiner.tunnel.connect(host.address);
    hub.inject("100.64.0.2", Message{.type = MessageType::answer, .from = "100.64.0.1", .sdp = "not a session description"});
    hub.inject("100.64.0.2", Message{.type = MessageType::answer, .from = "100.64.0.7", .sdp = "v=0"});

    CHECK_FALSE(eventually([&] { return joiner.tunnel.client_state() == LinkState::open; }, unanswered_wait));
    CHECK(joiner.tunnel.client_remote() == host.address);
}

TEST_CASE("a TURN server that never answers does not hold up the link") {
    const std::vector<IceServer> unreachable{
        IceServer{.urls = {"stun:192.0.2.1:3478"}},
        IceServer{.urls = {"turn:192.0.2.1:3478?transport=udp"}, .username = "user", .credential = "pass"},
    };
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1", unreachable);
    Cabinet joiner(hub, "100.64.0.2", unreachable);
    host.tunnel.start_accepting();

    const auto started = std::chrono::steady_clock::now();
    joiner.tunnel.connect(host.address);

    REQUIRE(eventually([&] { return joiner.tunnel.client_state() == LinkState::open; }, prompt_link));
    CHECK(eventually([&] { return host.tunnel.accept_into(0); }, prompt_link));
    CHECK(std::chrono::steady_clock::now() - started < prompt_link);
}

TEST_CASE("calls without a link fail cleanly") {
    LoopbackSignaling hub;
    Cabinet cabinet(hub, "100.64.0.1");

    CHECK_FALSE(cabinet.tunnel.send_on_client(frame(1)));
    CHECK(cabinet.tunnel.receive_on_client().status == Received::Status::closed);
    CHECK(cabinet.tunnel.client_state() == LinkState::closed);
    CHECK_FALSE(cabinet.tunnel.send_on_socket(3, frame(1)));
    CHECK(cabinet.tunnel.receive_on_socket(3).status == Received::Status::closed);
    CHECK_FALSE(cabinet.tunnel.send_datagram(*parse_address("100.64.0.5"), frame(1)));
    CHECK_FALSE(cabinet.tunnel.receive_datagram());
    CHECK_FALSE(cabinet.tunnel.accept_into(0));
    cabinet.tunnel.close_socket(3);
    cabinet.tunnel.disconnect_client();
}
