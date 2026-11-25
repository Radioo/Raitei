#include "virtual_network.h"

#include "core/ipv4.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

namespace raitei::iidx {

namespace {

std::int32_t hand_out(std::vector<std::uint8_t>& pending, std::uint8_t* buffer, std::size_t room, Received::Status last) {
    if (pending.empty()) {
        return last == Received::Status::closed ? peer_closed : would_block;
    }
    if (room == 0) {
        return would_block;
    }
    const auto size = std::min(pending.size(), room);
    std::memcpy(buffer, pending.data(), size);
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(size));
    return static_cast<std::int32_t>(size);
}

}

VirtualNetwork::VirtualNetwork(Tunnel& tunnel, const AvsNet& real) : tunnel_(tunnel), real_(real) {
}

std::uint16_t VirtualNetwork::bound_port(std::int32_t descriptor) const {
    std::scoped_lock lock(mutex_);
    const auto found = bound_ports_.find(descriptor);
    return found == bound_ports_.end() ? 0 : found->second;
}

bool VirtualNetwork::is_listener(std::int32_t descriptor) const {
    std::scoped_lock lock(mutex_);
    return listener_ == descriptor;
}

bool VirtualNetwork::is_tunnel_only(std::int32_t descriptor) const {
    std::scoped_lock lock(mutex_);
    return tunnel_only_.contains(descriptor);
}

std::int32_t VirtualNetwork::bind(std::int32_t descriptor, std::uint32_t address, std::uint16_t port) {
    const auto result = real_.bind(descriptor, address, port);
    std::scoped_lock lock(mutex_);
    bound_ports_[descriptor] = port;
    if (result < 0) {
        tunnel_only_.insert(descriptor);
        spdlog::warn("port {} is taken on this machine, the socket only carries tunnel traffic", port);
        return bound;
    }
    return result;
}

std::int32_t VirtualNetwork::listen(std::int32_t descriptor, std::int32_t backlog) {
    const auto result = is_tunnel_only(descriptor) ? bound : real_.listen(descriptor, backlog);
    if (result >= 0) {
        {
            std::scoped_lock lock(mutex_);
            listener_ = descriptor;
        }
        spdlog::info("game listens on port {}, tunnel links are accepted from now on", bound_port(descriptor));
        tunnel_.start_accepting();
    }
    return result;
}

std::int32_t VirtualNetwork::accept(std::int32_t descriptor, std::uint32_t* address, std::uint16_t* port) {
    if (is_listener(descriptor) && tunnel_.has_ready()) {
        const auto fresh = real_.socket(stream_socket);
        if (fresh >= 0 && tunnel_.accept_into(fresh)) {
            *address = tunnel_.remote(fresh).value_or(0);
            *port = bound_port(descriptor);
            return fresh;
        }
        if (fresh >= 0) {
            real_.close(fresh);
        }
    }
    return is_tunnel_only(descriptor) ? would_block : real_.accept(descriptor, address, port);
}

std::int32_t VirtualNetwork::connect(std::int32_t descriptor, std::uint32_t address, std::uint16_t port) {
    if (!is_virtual_address(address)) {
        note_outside_tunnel("connects to", address, port);
        return real_.connect(descriptor, address, port);
    }
    {
        std::scoped_lock lock(mutex_);
        pending_.erase(descriptor);
        finished_.erase(descriptor);
        reading_closed_.erase(descriptor);
    }
    tunnel_.connect(descriptor, address);
    return in_progress;
}

std::int32_t VirtualNetwork::poll(PollEntry* entries, std::uint32_t count, std::int32_t timeout) {
    const std::span polled(entries, count);
    std::vector<bool> tunnelled;
    std::vector<PollEntry> outside;
    for (const auto& entry : polled) {
        tunnelled.push_back(tunnel_.owns(entry.descriptor));
        if (!tunnelled.back()) {
            outside.push_back(entry);
        }
    }
    if (outside.size() == polled.size()) {
        return real_.poll(entries, count, timeout);
    }

    std::int32_t ready = 0;
    if (!outside.empty()) {
        ready = real_.poll(outside.data(), static_cast<std::uint32_t>(outside.size()), 0);
        if (ready < 0) {
            return ready;
        }
    }
    auto from_real = outside.begin();
    for (std::size_t index = 0; index < polled.size(); ++index) {
        auto& entry = polled[index];
        if (!tunnelled[index]) {
            entry = *from_real++;
            continue;
        }
        entry.revents = 0;
        entry.result = 0;
        switch (tunnel_.state(entry.descriptor)) {
        case LinkState::connecting:
            break;
        case LinkState::open:
            entry.revents = entry.events & poll_writable;
            break;
        case LinkState::closed:
            entry.revents = entry.events;
            entry.result = connection_refused;
            break;
        }
        if (entry.revents != 0) {
            ++ready;
        }
    }
    return ready;
}

std::int32_t VirtualNetwork::send(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length) {
    if (tunnel_.owns(descriptor)) {
        return tunnel_.send(descriptor, std::span(buffer, static_cast<std::size_t>(length))) ? length : not_connected;
    }
    {
        std::scoped_lock lock(mutex_);
        if (finished_.contains(descriptor)) {
            return not_connected;
        }
    }
    return real_.send(descriptor, buffer, length);
}

Received::Status VirtualNetwork::gather(std::int32_t descriptor, std::vector<std::uint8_t>& pending, std::size_t room) {
    auto last = Received::Status::frame;
    while (pending.size() < room && last == Received::Status::frame) {
        auto received = tunnel_.receive(descriptor);
        last = received.status;
        pending.insert(pending.end(), received.bytes.begin(), received.bytes.end());
    }
    return last;
}

std::int32_t VirtualNetwork::recv(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity) {
    if (!tunnel_.owns(descriptor)) {
        {
            std::scoped_lock lock(mutex_);
            if (finished_.contains(descriptor)) {
                return hand_out(pending_[descriptor], buffer, static_cast<std::size_t>(std::max(capacity, 0)), Received::Status::closed);
            }
        }
        return real_.recv(descriptor, buffer, capacity);
    }

    std::scoped_lock lock(mutex_);
    if (reading_closed_.contains(descriptor)) {
        std::vector<std::uint8_t> discarded;
        gather(descriptor, discarded, std::numeric_limits<std::size_t>::max());
        return peer_closed;
    }
    const auto room = static_cast<std::size_t>(std::max(capacity, 0));
    auto& pending = pending_[descriptor];
    const auto last = gather(descriptor, pending, room);
    return hand_out(pending, buffer, room, last);
}

void VirtualNetwork::note_outside_tunnel(std::string_view what, std::uint32_t address, std::uint16_t port) {
    std::scoped_lock lock(mutex_);
    if (outside_tunnel_.emplace(address, port).second) {
        spdlog::info("the game {} {}:{} outside the tunnel", what, format_address(address), port);
    }
}

std::int32_t VirtualNetwork::sendto(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length, std::uint32_t address, std::uint16_t port) {
    if (!is_virtual_address(address)) {
        note_outside_tunnel("sends datagrams to", address, port);
        return real_.sendto(descriptor, buffer, length, address, port);
    }
    tunnel_.send_datagram(address, std::span(buffer, static_cast<std::size_t>(length)));
    return length;
}

std::int32_t VirtualNetwork::recvfrom(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity, std::uint32_t* address, std::uint16_t* port) {
    while (auto datagram = tunnel_.receive_datagram()) {
        if (datagram->bytes.size() > static_cast<std::size_t>(std::max(capacity, 0))) {
            spdlog::warn("dropping a {} byte datagram from {}, the game reads at most {}", datagram->bytes.size(), format_address(datagram->from), capacity);
            continue;
        }
        std::memcpy(buffer, datagram->bytes.data(), datagram->bytes.size());
        *address = datagram->from;
        *port = bound_port(descriptor);
        return static_cast<std::int32_t>(datagram->bytes.size());
    }
    if (is_tunnel_only(descriptor)) {
        return would_block;
    }
    const auto received = real_.recvfrom(descriptor, buffer, capacity, address, port);
    if (received > 0) {
        note_outside_tunnel("receives datagrams from", *address, *port);
    }
    return received;
}

std::int32_t VirtualNetwork::shutdown(std::int32_t descriptor, std::int32_t how) {
    if (!tunnel_.owns(descriptor)) {
        return real_.shutdown(descriptor, how);
    }
    {
        std::scoped_lock lock(mutex_);
        if (how == shutdown_reading) {
            pending_.erase(descriptor);
            reading_closed_.insert(descriptor);
            return 0;
        }
        gather(descriptor, pending_[descriptor], std::numeric_limits<std::size_t>::max());
        finished_.insert(descriptor);
    }
    tunnel_.close(descriptor);
    return 0;
}

std::int32_t VirtualNetwork::close(std::int32_t descriptor) {
    bool was_listener = false;
    {
        std::scoped_lock lock(mutex_);
        pending_.erase(descriptor);
        finished_.erase(descriptor);
        reading_closed_.erase(descriptor);
        tunnel_only_.erase(descriptor);
        bound_ports_.erase(descriptor);
        if (listener_ == descriptor) {
            listener_.reset();
            was_listener = true;
        }
    }
    if (was_listener) {
        tunnel_.stop_accepting();
    }
    tunnel_.close(descriptor);
    return real_.close(descriptor);
}

}
