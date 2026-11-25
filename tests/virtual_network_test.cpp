#include "loopback_signaling.h"

#include "core/ipv4.h"
#include "games/iidx/virtual_network.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

using namespace raitei;
using namespace raitei::iidx;
using raitei::testing::eventually;
using raitei::testing::LoopbackSignaling;

namespace {

constexpr std::uint16_t game_port = 5700;
constexpr std::int32_t listener = 5;
constexpr std::int32_t udp_socket = 9;
constexpr std::int32_t first_fresh_descriptor = 100;
constexpr std::int32_t real_result = 77;
constexpr int dialled = -1;
constexpr std::size_t burst_frames = 50;
constexpr std::size_t burst_frame_size = 3;
constexpr std::int32_t large_send = 16 * 1024;
constexpr int large_send_count = 100;
constexpr auto nobody = "100.64.0.99";

constexpr std::int32_t address_in_use = static_cast<std::int32_t>(0x80080062u);

struct FakeAvs {
    std::int32_t bind_result = 0;
    std::int32_t real_accepts = 0;
    std::int32_t next_descriptor = first_fresh_descriptor;
    std::vector<std::int32_t> closed;
    std::vector<std::uint32_t> real_connects;
    std::vector<std::uint32_t> real_sends_to;
    std::vector<std::int32_t> polled;
    std::int32_t poll_timeout = 0;
};

FakeAvs fake;

AvsNet fake_net() {
    fake = FakeAvs{};
    return AvsNet{
        .socket = [](std::int32_t) { return fake.next_descriptor++; },
        .bind = [](std::int32_t, std::uint32_t, std::uint16_t) { return fake.bind_result; },
        .listen = [](std::int32_t, std::int32_t) { return 0; },
        .accept = [](std::int32_t, std::uint32_t*, std::uint16_t*) {
            ++fake.real_accepts;
            return would_block;
        },
        .connect = [](std::int32_t, std::uint32_t address, std::uint16_t) {
            fake.real_connects.push_back(address);
            return real_result;
        },
        .poll = [](PollEntry* entries, std::uint32_t count, std::int32_t timeout) {
            fake.poll_timeout = timeout;
            for (std::uint32_t index = 0; index < count; ++index) {
                fake.polled.push_back(entries[index].descriptor);
                entries[index].revents = entries[index].events;
            }
            return real_result;
        },
        .send = [](std::int32_t, const std::uint8_t*, std::int32_t) { return real_result; },
        .recv = [](std::int32_t, std::uint8_t*, std::int32_t) { return real_result; },
        .sendto = [](std::int32_t, const std::uint8_t*, std::int32_t length, std::uint32_t address, std::uint16_t) {
            fake.real_sends_to.push_back(address);
            return length;
        },
        .recvfrom = [](std::int32_t, std::uint8_t*, std::int32_t, std::uint32_t*, std::uint16_t*) { return would_block; },
        .shutdown = [](std::int32_t, std::int32_t) { return real_result; },
        .close = [](std::int32_t descriptor) {
            fake.closed.push_back(descriptor);
            return 0;
        },
    };
}

struct Cabinet {
    Cabinet(LoopbackSignaling& hub, const std::string& address, const AvsNet& real)
        : signaling(hub.join(address)), address(*parse_address(address)), tunnel(signaling), network(tunnel, real) {
    }

    LoopbackSignaling::Endpoint& signaling;
    std::uint32_t address;
    Tunnel tunnel;
    VirtualNetwork network;
};

std::int32_t accept_tunnelled(Cabinet& host, std::uint32_t* address, std::uint16_t* port) {
    std::int32_t accepted = would_block;
    eventually([&] {
        accepted = host.network.accept(listener, address, port);
        return accepted != would_block;
    });
    return accepted;
}

bool writable(Cabinet& cabinet, std::int32_t descriptor) {
    PollEntry entry{.descriptor = descriptor, .events = poll_writable, .revents = 0, .result = 0};
    return cabinet.network.poll(&entry, 1, -1) == 1 && entry.revents == poll_writable && entry.result == 0;
}

std::vector<std::uint8_t> bytes(std::size_t count, std::uint8_t fill) {
    return std::vector<std::uint8_t>(count, fill);
}

}

TEST_CASE("a tunnelled joiner is accepted as a fresh avs socket named after its virtual address") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet joiner(hub, "100.64.0.2", real);
    REQUIRE(host.network.bind(listener, 0, game_port) == 0);
    REQUIRE(host.network.listen(listener, 32) == 0);

    std::uint32_t address = 0;
    std::uint16_t port = 0;
    CHECK(host.network.accept(listener, &address, &port) == would_block);

    joiner.tunnel.connect(dialled, host.address);
    const auto accepted = accept_tunnelled(host, &address, &port);

    CHECK(accepted == first_fresh_descriptor);
    CHECK(address == joiner.address);
    CHECK(port == game_port);
}

TEST_CASE("a virtual connect stays pending until the link opens, then poll says writable") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();

    CHECK(guest.network.connect(8, *parse_address(nobody), game_port) == in_progress);
    PollEntry waiting{.descriptor = 8, .events = poll_writable, .revents = 0, .result = 0};
    CHECK(guest.network.poll(&waiting, 1, -1) == 0);
    CHECK(waiting.revents == 0);
    CHECK(waiting.result == 0);

    CHECK(guest.network.connect(7, host.address, game_port) == in_progress);
    CHECK(fake.real_connects.empty());
    REQUIRE(eventually([&] { return writable(guest, 7); }));

    const auto hello = bytes(12, 3);
    CHECK(guest.network.send(7, hello.data(), static_cast<std::int32_t>(hello.size())) == 12);
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));
    std::optional<std::vector<std::uint8_t>> received;
    REQUIRE(eventually([&] {
        auto frame = host.tunnel.receive(0);
        if (frame.status == Received::Status::frame) {
            received = std::move(frame.bytes);
        }
        return received.has_value();
    }));
    CHECK(*received == hello);
}

TEST_CASE("real addresses and real sockets go to avs untouched") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet cabinet(hub, "100.64.0.1", real);
    const auto lan = *parse_address("192.168.10.20");
    std::array<std::uint8_t, 4> buffer{};

    CHECK(cabinet.network.connect(7, lan, game_port) == real_result);
    CHECK(fake.real_connects == std::vector<std::uint32_t>{lan});
    PollEntry entry{.descriptor = 7, .events = poll_writable, .revents = 0, .result = 0};
    CHECK(cabinet.network.poll(&entry, 1, -1) == real_result);
    CHECK(cabinet.network.send(7, buffer.data(), 4) == real_result);
    CHECK(cabinet.network.recv(7, buffer.data(), 4) == real_result);
    CHECK(cabinet.network.shutdown(7, 2) == real_result);
    CHECK(cabinet.network.sendto(udp_socket, buffer.data(), 4, lan, game_port) == 4);
    CHECK(fake.real_sends_to == std::vector<std::uint32_t>{lan});
}

TEST_CASE("a stream read hands out a message across several small reads") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet joiner(hub, "100.64.0.2", real);
    host.network.bind(listener, 0, game_port);
    host.network.listen(listener, 32);
    joiner.tunnel.connect(dialled, host.address);
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    const auto accepted = accept_tunnelled(host, &address, &port);
    REQUIRE(accepted == first_fresh_descriptor);

    REQUIRE(joiner.tunnel.send(dialled, bytes(10, 9)));
    std::array<std::uint8_t, 4> buffer{};
    std::int32_t first = would_block;
    REQUIRE(eventually([&] { return (first = host.network.recv(accepted, buffer.data(), 4)) != would_block; }));
    CHECK(first == 4);
    CHECK(host.network.recv(accepted, buffer.data(), 4) == 4);
    CHECK(host.network.recv(accepted, buffer.data(), 4) == 2);
    CHECK(buffer[1] == 9);
    CHECK(host.network.recv(accepted, buffer.data(), 4) == would_block);
}

TEST_CASE("messages already queued come back together from one read") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet joiner(hub, "100.64.0.2", real);
    host.network.bind(listener, 0, game_port);
    host.network.listen(listener, 32);
    joiner.tunnel.connect(dialled, host.address);
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    const auto accepted = accept_tunnelled(host, &address, &port);
    REQUIRE(accepted == first_fresh_descriptor);

    for (std::size_t index = 0; index < burst_frames; ++index) {
        REQUIRE(joiner.tunnel.send(dialled, bytes(burst_frame_size, static_cast<std::uint8_t>(index))));
    }
    std::vector<std::uint8_t> received;
    std::int32_t largest_read = 0;
    std::array<std::uint8_t, 1024> buffer{};
    REQUIRE(eventually([&] {
        const auto read = host.network.recv(accepted, buffer.data(), static_cast<std::int32_t>(buffer.size()));
        if (read > 0) {
            received.insert(received.end(), buffer.begin(), buffer.begin() + read);
            largest_read = std::max(largest_read, read);
        }
        return received.size() == burst_frames * burst_frame_size;
    }));
    CHECK(largest_read > static_cast<std::int32_t>(burst_frame_size));
    for (std::size_t index = 0; index < burst_frames; ++index) {
        CHECK(received[index * burst_frame_size] == static_cast<std::uint8_t>(index));
    }
}

TEST_CASE("shutting down a tunnelled socket closes the far side and reads as closed") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    CHECK(guest.network.shutdown(7, 2) == 0);
    CHECK(eventually([&] { return host.tunnel.receive(0).status == Received::Status::closed; }));
    std::array<std::uint8_t, 4> buffer{};
    CHECK(guest.network.recv(7, buffer.data(), 4) == peer_closed);
    CHECK(guest.network.send(7, buffer.data(), 4) == not_connected);
    CHECK(guest.network.close(7) == 0);
    CHECK(fake.closed == std::vector<std::int32_t>{7});
}

TEST_CASE("a link the far side drops polls as a refused connection and reads as closed") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    host.tunnel.close(0);

    std::array<std::uint8_t, 4> buffer{};
    CHECK(eventually([&] { return guest.network.recv(7, buffer.data(), 4) == peer_closed; }));
    PollEntry entry{.descriptor = 7, .events = poll_writable, .revents = 0, .result = 0};
    CHECK(guest.network.poll(&entry, 1, -1) == 1);
    CHECK(entry.revents == poll_writable);
    CHECK(entry.result == connection_refused);
}

TEST_CASE("datagrams to a virtual address cross the tunnel and arrive from the game port") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.network.bind(listener, 0, game_port);
    host.network.listen(listener, 32);
    host.network.bind(udp_socket, 0, game_port);
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));

    const auto datagram = bytes(20, 6);
    std::array<std::uint8_t, 64> buffer{};
    std::uint32_t from = 0;
    std::uint16_t port = 0;
    std::int32_t size = would_block;
    REQUIRE(eventually([&] {
        guest.network.sendto(udp_socket, datagram.data(), static_cast<std::int32_t>(datagram.size()), host.address, game_port);
        size = host.network.recvfrom(udp_socket, buffer.data(), static_cast<std::int32_t>(buffer.size()), &from, &port);
        return size != would_block;
    }));
    CHECK(size == 20);
    CHECK(from == guest.address);
    CHECK(port == game_port);
    CHECK(buffer[19] == 6);
    CHECK(fake.real_sends_to.empty());
}

TEST_CASE("closing the listening socket drops every incoming link") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet joiner(hub, "100.64.0.2", real);
    host.network.bind(listener, 0, game_port);
    host.network.listen(listener, 32);
    joiner.tunnel.connect(dialled, host.address);
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    REQUIRE(accept_tunnelled(host, &address, &port) == first_fresh_descriptor);

    host.network.close(listener);

    CHECK(eventually([&] { return joiner.tunnel.receive(dialled).status == Received::Status::closed; }));
    CHECK_FALSE(host.tunnel.owns(first_fresh_descriptor));
}

TEST_CASE("a game port another process holds still carries tunnelled joiners") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    fake.bind_result = address_in_use;
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet joiner(hub, "100.64.0.2", real);

    CHECK(host.network.bind(listener, 0, game_port) == bound);
    CHECK(host.network.listen(listener, 32) == bound);
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    CHECK(host.network.accept(listener, &address, &port) == would_block);
    CHECK(fake.real_accepts == 0);

    joiner.tunnel.connect(dialled, host.address);
    CHECK(accept_tunnelled(host, &address, &port) == first_fresh_descriptor);
    CHECK(address == joiner.address);
}

TEST_CASE("large sends in a burst report their full length") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    const auto block = bytes(static_cast<std::size_t>(large_send), 4);
    for (int index = 0; index < large_send_count; ++index) {
        REQUIRE(guest.network.send(7, block.data(), large_send) == large_send);
    }
    std::size_t arrived = 0;
    REQUIRE(eventually([&] {
        auto frame = host.tunnel.receive(0);
        arrived += frame.bytes.size();
        return arrived == static_cast<std::size_t>(large_send) * large_send_count;
    }));
}

TEST_CASE("bytes that arrived before the far side closed are read before the close") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    REQUIRE(host.tunnel.send(0, bytes(6, 5)));
    std::array<std::uint8_t, 64> buffer{};
    REQUIRE(eventually([&] { return guest.network.recv(7, buffer.data(), 1) == 1; }));
    host.tunnel.close(0);

    std::vector<std::int32_t> reads;
    REQUIRE(eventually([&] {
        const auto read = guest.network.recv(7, buffer.data(), static_cast<std::int32_t>(buffer.size()));
        if (read != would_block) {
            reads.push_back(read);
        }
        return read == peer_closed;
    }));
    CHECK(reads == std::vector<std::int32_t>{5, peer_closed});
}

TEST_CASE("bytes already in hand survive a full shutdown") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    REQUIRE(host.tunnel.send(0, bytes(6, 5)));
    std::array<std::uint8_t, 64> buffer{};
    REQUIRE(eventually([&] { return guest.network.recv(7, buffer.data(), 1) == 1; }));

    CHECK(guest.network.shutdown(7, 2) == 0);
    CHECK(guest.network.recv(7, buffer.data(), static_cast<std::int32_t>(buffer.size())) == 5);
    CHECK(guest.network.recv(7, buffer.data(), static_cast<std::int32_t>(buffer.size())) == peer_closed);
    CHECK(eventually([&] { return host.tunnel.receive(0).status == Received::Status::closed; }));
}

TEST_CASE("shutting down only reading keeps the link open for sending") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    CHECK(guest.network.shutdown(7, shutdown_reading) == 0);
    std::array<std::uint8_t, 4> buffer{};
    CHECK(guest.network.recv(7, buffer.data(), 4) == peer_closed);
    CHECK(guest.network.send(7, buffer.data(), 4) == 4);
    std::optional<std::vector<std::uint8_t>> received;
    REQUIRE(eventually([&] {
        auto frame = host.tunnel.receive(0);
        if (frame.status == Received::Status::frame) {
            received = std::move(frame.bytes);
        }
        return received.has_value();
    }));
    CHECK(received->size() == 4);
}

TEST_CASE("a read with no room never looks like a closed link") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));
    REQUIRE(eventually([&] { return host.tunnel.accept_into(0); }));

    REQUIRE(host.tunnel.send(0, bytes(6, 5)));
    std::array<std::uint8_t, 4> buffer{};
    REQUIRE(eventually([&] { return guest.network.recv(7, buffer.data(), 1) == 1; }));
    CHECK(guest.network.recv(7, buffer.data(), 0) == would_block);
    CHECK(guest.network.recv(7, buffer.data(), 4) == 4);
}

TEST_CASE("a poll over tunnelled and real sockets asks avs only about the real ones") {
    LoopbackSignaling hub;
    const auto real = fake_net();
    Cabinet host(hub, "100.64.0.1", real);
    Cabinet guest(hub, "100.64.0.2", real);
    host.tunnel.start_accepting();
    guest.network.connect(7, host.address, game_port);
    REQUIRE(eventually([&] { return writable(guest, 7); }));

    std::array entries{
        PollEntry{.descriptor = 7, .events = poll_writable, .revents = 0, .result = 0},
        PollEntry{.descriptor = 8, .events = poll_writable, .revents = 0, .result = 0},
    };
    CHECK(guest.network.poll(entries.data(), static_cast<std::uint32_t>(entries.size()), -1) == real_result + 1);
    CHECK(fake.polled == std::vector<std::int32_t>{8});
    CHECK(fake.poll_timeout == 0);
    CHECK(entries[0].revents == poll_writable);
    CHECK(entries[1].revents == poll_writable);
}
