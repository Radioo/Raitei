#include "loopback_signaling.h"

#include "core/ipv4.h"
#include "core/tunnel.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace raitei;
using raitei::testing::eventually;
using raitei::testing::LoopbackSignaling;

namespace {

constexpr int dialled = -1;
constexpr std::size_t frame_size = 48;
constexpr auto unanswered_wait = std::chrono::seconds{2};
constexpr auto prompt_link = std::chrono::seconds{5};
constexpr auto stray_link = "00000000000000ff";
constexpr std::size_t large_frame_size = 16 * 1024;
constexpr int large_frame_count = 200;

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
    joiner.tunnel.connect(dialled, host.address);
    REQUIRE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(socket); }));
}

}

TEST_CASE("a joiner reaches a host and frames flow both ways") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    CHECK(host.tunnel.owns(0));
    CHECK(joiner.tunnel.remote(dialled) == host.address);

    REQUIRE(joiner.tunnel.send(dialled, frame(1)));
    CHECK(next_frame([&] { return host.tunnel.receive(0); }) == frame(1));

    REQUIRE(host.tunnel.send(0, frame(2)));
    CHECK(next_frame([&] { return joiner.tunnel.receive(dialled); }) == frame(2));
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
                host.tunnel.send(0, frame(5));
                host.tunnel.send_datagram(joiner.address, frame(5));
            } catch (...) {
                threw = true;
            }
        }
    });

    joiner.tunnel.close(dialled);
    CHECK(closes([&] { return host.tunnel.receive(0); }));
    sender.request_stop();
    sender.join();

    CHECK_FALSE(threw);
    CHECK_FALSE(host.tunnel.send(0, frame(6)));
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
                joiner.tunnel.send(dialled, frame(7));
                joiner.tunnel.send_datagram(host.address, frame(7));
            } catch (...) {
                threw = true;
            }
        }
    });

    host.tunnel.close(0);
    CHECK(closes([&] { return joiner.tunnel.receive(dialled); }));
    sender.request_stop();
    sender.join();

    CHECK_FALSE(threw);
    CHECK_FALSE(host.tunnel.owns(0));
}

TEST_CASE("stop accepting closes every joiner's link") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet first(hub, "100.64.0.2");
    Cabinet second(hub, "100.64.0.3");
    link(host, first, 0);
    link(host, second, 1);

    host.tunnel.stop_accepting();

    CHECK(closes([&] { return first.tunnel.receive(dialled); }));
    CHECK(closes([&] { return second.tunnel.receive(dialled); }));
    CHECK_FALSE(host.tunnel.owns(0));
    CHECK_FALSE(host.tunnel.owns(1));
}

TEST_CASE("an offer that arrives while the host is not accepting is ignored") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");

    joiner.tunnel.connect(dialled, host.address);

    CHECK_FALSE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }, unanswered_wait));
    CHECK_FALSE(host.tunnel.accept_into(0));
}

TEST_CASE("a joiner can link to the same host again after disconnecting") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    joiner.tunnel.close(dialled);
    REQUIRE(closes([&] { return host.tunnel.receive(0); }));
    host.tunnel.close(0);

    link(host, joiner, 1);
    REQUIRE(joiner.tunnel.send(dialled, frame(8)));
    CHECK(next_frame([&] { return host.tunnel.receive(1); }) == frame(8));
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
        REQUIRE(joiners[index]->tunnel.send(dialled, frame(static_cast<std::uint8_t>(10 + index))));
    }
    for (int socket = 0; socket < static_cast<int>(joiners.size()); ++socket) {
        const auto received = next_frame([&] { return host.tunnel.receive(socket); });
        REQUIRE(received);
        fills_at_host.insert(received->front());
    }
    CHECK(fills_at_host == std::set<std::uint8_t>{10, 11, 12});

    for (int socket = 0; socket < static_cast<int>(joiners.size()); ++socket) {
        REQUIRE(host.tunnel.send(socket, frame(static_cast<std::uint8_t>(20 + socket))));
    }
    std::set<std::uint8_t> fills_at_joiners;
    for (const auto& joiner : joiners) {
        const auto received = next_frame([&] { return joiner->tunnel.receive(dialled); });
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

    hub.inject("100.64.0.1", Message{.type = MessageType::offer, .from = "100.64.0.9", .link = stray_link, .sdp = "not a session description"});
    hub.inject("100.64.0.1", Message{.type = MessageType::offer, .from = "not an address", .link = stray_link, .sdp = "v=0"});
    hub.inject("100.64.0.1", Message{.type = MessageType::answer, .from = "100.64.0.9", .link = stray_link, .sdp = "v=0"});
    hub.inject("100.64.0.1", Message{.type = MessageType::offer, .from = "100.64.0.9", .sdp = "v=0"});

    link(host, joiner, 0);
    REQUIRE(joiner.tunnel.send(dialled, frame(9)));
    CHECK(next_frame([&] { return host.tunnel.receive(0); }) == frame(9));
}

TEST_CASE("a malformed answer from the dialled host leaves the joiner connecting") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");

    joiner.tunnel.connect(dialled, host.address);
    REQUIRE(eventually([&] { return !hub.delivered_to("100.64.0.1").empty(); }));
    const auto link_id = hub.delivered_to("100.64.0.1").front().link;
    hub.inject("100.64.0.2", Message{.type = MessageType::answer, .from = "100.64.0.1", .link = link_id, .sdp = "not a session description"});
    hub.inject("100.64.0.2", Message{.type = MessageType::answer, .from = "100.64.0.7", .link = link_id, .sdp = "v=0"});

    CHECK_FALSE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }, unanswered_wait));
    CHECK(joiner.tunnel.remote(dialled) == host.address);
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
    joiner.tunnel.connect(dialled, host.address);

    REQUIRE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }, prompt_link));
    CHECK(eventually([&] { return host.tunnel.accept_into(0); }, prompt_link));
    CHECK(std::chrono::steady_clock::now() - started < prompt_link);
}

TEST_CASE("calls without a link fail cleanly") {
    LoopbackSignaling hub;
    Cabinet cabinet(hub, "100.64.0.1");

    CHECK_FALSE(cabinet.tunnel.send(dialled, frame(1)));
    CHECK(cabinet.tunnel.receive(dialled).status == Received::Status::closed);
    CHECK(cabinet.tunnel.state(dialled) == LinkState::closed);
    CHECK_FALSE(cabinet.tunnel.send(3, frame(1)));
    CHECK(cabinet.tunnel.receive(3).status == Received::Status::closed);
    CHECK_FALSE(cabinet.tunnel.send_datagram(*parse_address("100.64.0.5"), frame(1)));
    CHECK_FALSE(cabinet.tunnel.receive_datagram());
    CHECK_FALSE(cabinet.tunnel.accept_into(0));
    cabinet.tunnel.close(3);
    cabinet.tunnel.close(dialled);
}

TEST_CASE("two cabinets that dial each other at once get two working links") {
    LoopbackSignaling hub;
    Cabinet first(hub, "100.64.0.1");
    Cabinet second(hub, "100.64.0.2");
    first.tunnel.start_accepting();
    second.tunnel.start_accepting();

    first.tunnel.connect(10, second.address);
    second.tunnel.connect(20, first.address);

    REQUIRE(eventually([&] { return first.tunnel.state(10) == LinkState::open; }));
    REQUIRE(eventually([&] { return second.tunnel.state(20) == LinkState::open; }));
    REQUIRE(eventually([&] { return first.tunnel.accept_into(11); }));
    REQUIRE(eventually([&] { return second.tunnel.accept_into(21); }));

    REQUIRE(first.tunnel.send(10, frame(30)));
    CHECK(next_frame([&] { return second.tunnel.receive(21); }) == frame(30));
    REQUIRE(second.tunnel.send(20, frame(31)));
    CHECK(next_frame([&] { return first.tunnel.receive(11); }) == frame(31));
}

TEST_CASE("one cabinet dials two hosts and keeps both links apart") {
    LoopbackSignaling hub;
    Cabinet left(hub, "100.64.0.1");
    Cabinet right(hub, "100.64.0.2");
    Cabinet guest(hub, "100.64.0.3");
    left.tunnel.start_accepting();
    right.tunnel.start_accepting();

    guest.tunnel.connect(1, left.address);
    guest.tunnel.connect(2, right.address);
    REQUIRE(eventually([&] { return guest.tunnel.state(1) == LinkState::open && guest.tunnel.state(2) == LinkState::open; }));
    REQUIRE(eventually([&] { return left.tunnel.accept_into(0); }));
    REQUIRE(eventually([&] { return right.tunnel.accept_into(0); }));
    CHECK(guest.tunnel.remote(1) == left.address);
    CHECK(guest.tunnel.remote(2) == right.address);

    REQUIRE(guest.tunnel.send(1, frame(40)));
    REQUIRE(guest.tunnel.send(2, frame(41)));
    CHECK(next_frame([&] { return left.tunnel.receive(0); }) == frame(40));
    CHECK(next_frame([&] { return right.tunnel.receive(0); }) == frame(41));

    guest.tunnel.close(1);
    CHECK(closes([&] { return left.tunnel.receive(0); }));
    REQUIRE(guest.tunnel.send(2, frame(42)));
    CHECK(next_frame([&] { return right.tunnel.receive(0); }) == frame(42));
}

TEST_CASE("a burst of large frames is accepted whole and arrives in order") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);

    for (int index = 0; index < large_frame_count; ++index) {
        REQUIRE(joiner.tunnel.send(dialled, std::vector<std::uint8_t>(large_frame_size, static_cast<std::uint8_t>(index))));
    }

    for (int index = 0; index < large_frame_count; ++index) {
        const auto received = next_frame([&] { return host.tunnel.receive(0); });
        REQUIRE(received);
        REQUIRE(received->size() == large_frame_size);
        CHECK(received->front() == static_cast<std::uint8_t>(index));
    }
}

TEST_CASE("two links to the same host each get the answer to their own offer") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet guest(hub, "100.64.0.2");
    host.tunnel.start_accepting();

    guest.tunnel.connect(1, host.address);
    guest.tunnel.connect(2, host.address);
    REQUIRE(eventually([&] { return guest.tunnel.state(1) == LinkState::open && guest.tunnel.state(2) == LinkState::open; }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(10); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(11); }));

    REQUIRE(guest.tunnel.send(1, frame(50)));
    REQUIRE(guest.tunnel.send(2, frame(51)));
    const auto at_first = next_frame([&] { return host.tunnel.receive(10); });
    const auto at_second = next_frame([&] { return host.tunnel.receive(11); });
    REQUIRE(at_first);
    REQUIRE(at_second);
    CHECK(std::set<std::uint8_t>{at_first->front(), at_second->front()} == std::set<std::uint8_t>{50, 51});
}

TEST_CASE("an answer to an earlier dial does not take over the next dial to the same host") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    link(host, joiner, 0);
    const auto answers = hub.delivered_to("100.64.0.2");
    const auto earlier = std::ranges::find_if(answers, [](const Message& message) { return message.type == MessageType::answer; });
    REQUIRE(earlier != answers.end());

    joiner.tunnel.close(dialled);
    host.tunnel.close(0);
    joiner.tunnel.connect(dialled, host.address);
    hub.inject("100.64.0.2", *earlier);

    REQUIRE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }, prompt_link));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));
    REQUIRE(joiner.tunnel.send(dialled, frame(60)));
    CHECK(next_frame([&] { return host.tunnel.receive(0); }) == frame(60));
}

TEST_CASE("a joiner that drops before the host accepts it is forgotten") {
    LoopbackSignaling hub;
    Cabinet host(hub, "100.64.0.1");
    Cabinet joiner(hub, "100.64.0.2");
    host.tunnel.start_accepting();
    joiner.tunnel.connect(dialled, host.address);
    REQUIRE(eventually([&] { return joiner.tunnel.state(dialled) == LinkState::open; }));
    REQUIRE(eventually([&] { return host.tunnel.has_ready(); }));

    joiner.tunnel.close(dialled);

    CHECK(eventually([&] { return !host.tunnel.has_ready(); }));
    CHECK_FALSE(host.tunnel.send_datagram(joiner.address, frame(70)));
}
