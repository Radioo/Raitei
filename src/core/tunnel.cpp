#include "tunnel.h"

#include "ipv4.h"

#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <variant>

namespace raitei {

namespace {

constexpr std::string_view stream_label = "stream";
constexpr std::string_view datagram_label = "datagrams";

rtc::binary to_binary(std::span<const std::uint8_t> bytes) {
    rtc::binary result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

bool send_binary(rtc::DataChannel& channel, std::span<const std::uint8_t> bytes) {
    try {
        channel.send(to_binary(bytes));
        return true;
    } catch (const std::exception& error) {
        spdlog::warn("send on {} failed, the link is closing: {}", channel.label(), error.what());
        return false;
    }
}

std::vector<std::uint8_t> to_bytes(const rtc::binary& bytes) {
    std::vector<std::uint8_t> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

rtc::Configuration peer_configuration(const std::optional<std::vector<IceServer>>& servers) {
    rtc::Configuration configuration;
    if (!servers) {
        spdlog::warn("no ice servers from the server, only a direct connection on the same network can form");
        return configuration;
    }
    spdlog::info("using {} ice servers", servers->size());

    for (const auto& server : *servers) {
        for (const auto& url : server.urls) {
            try {
                rtc::IceServer ice(url);
                if (ice.type == rtc::IceServer::Type::Turn) {
                    if (ice.relayType != rtc::IceServer::RelayType::TurnUdp) {
                        continue;
                    }
                    ice.username = server.username;
                    ice.password = server.credential;
                }
                configuration.iceServers.push_back(std::move(ice));
            } catch (const std::exception& error) {
                spdlog::warn("skipping ice server {}: {}", url, error.what());
            }
        }
    }
    return configuration;
}

}

struct Tunnel::Peer {
    std::uint32_t remote = 0;
    std::string link;
    bool offerer = false;
    bool answered = false;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> stream;
    std::shared_ptr<rtc::DataChannel> datagrams;
    std::deque<std::vector<std::uint8_t>> inbox;
    LinkState state = LinkState::connecting;
};

Tunnel::Tunnel(SignalingLink& signaling) : signaling_(signaling), link_ids_(std::random_device{}()), thread_([this](std::stop_token stop) { run(stop); }) {
    signaling_.on_message([this](Message message) {
        std::scoped_lock lock(mutex_);
        inbox_.push_back(std::move(message));
        wake_.notify_all();
    });
}

Tunnel::~Tunnel() {
    thread_.request_stop();
    thread_.join();
    signaling_.on_message(nullptr);

    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::scoped_lock lock(mutex_);
        peers = std::exchange(incoming_, {});
        peers.insert(peers.end(), released_.begin(), released_.end());
        released_.clear();
        for (const auto& [handle, peer] : handles_) {
            peers.push_back(peer);
        }
        handles_.clear();
        ready_.clear();
        unopened_.clear();
    }
    for (const auto& peer : peers) {
        for (const auto& channel : {peer->stream, peer->datagrams}) {
            if (channel) {
                channel->resetCallbacks();
            }
        }
        if (peer->connection) {
            peer->connection->resetCallbacks();
            peer->connection->close();
        }
    }
}

std::shared_ptr<rtc::PeerConnection> Tunnel::open_connection(const std::shared_ptr<Peer>& peer, const rtc::Configuration& configuration) {
    auto connection = std::make_shared<rtc::PeerConnection>(configuration);
    {
        std::scoped_lock lock(mutex_);
        peer->connection = connection;
    }

    const std::weak_ptr<Peer> weak = peer;
    connection->onLocalDescription([this, weak](rtc::Description description) {
        const auto locked = weak.lock();
        if (!locked) {
            return;
        }
        const auto to = format_address(locked->remote);
        const bool offer = description.type() == rtc::Description::Type::Offer;
        const auto kind = offer ? "offer" : "answer";
        if (signaling_.send(offer ? MessageType::offer : MessageType::answer, to, locked->link, std::string(description))) {
            spdlog::info("sent {} to {}", kind, to);
        } else {
            spdlog::warn("could not send {} to {}: signaling is not connected", kind, to);
        }
    });
    connection->onLocalCandidate([this, weak](rtc::Candidate candidate) {
        const auto locked = weak.lock();
        if (!locked) {
            return;
        }
        if (!signaling_.send_candidate(format_address(locked->remote), locked->link, std::string(candidate), candidate.mid(), locked->offerer)) {
            spdlog::warn("could not send a candidate to {}: signaling is not connected", format_address(locked->remote));
        }
    });
    connection->onStateChange([this, weak](rtc::PeerConnection::State state) {
        if (state != rtc::PeerConnection::State::Failed && state != rtc::PeerConnection::State::Closed) {
            return;
        }
        if (const auto locked = weak.lock()) {
            mark_closed(locked);
        }
    });
    return connection;
}

void Tunnel::open_outgoing(const std::shared_ptr<Peer>& peer) {
    const auto wanted = [this, &peer] {
        std::scoped_lock lock(mutex_);
        return std::ranges::any_of(handles_, [&peer](const auto& entry) { return entry.second == peer; });
    };
    if (!wanted()) {
        return;
    }

    try {
        const auto connection = open_connection(peer, peer_configuration(signaling_.ice_servers()));
        if (!wanted()) {
            connection->close();
            return;
        }

        rtc::DataChannelInit unreliable;
        unreliable.reliability.unordered = true;
        unreliable.reliability.maxRetransmits = 0;
        attach_stream(peer, connection->createDataChannel(std::string(stream_label)));
        attach_datagrams(peer, connection->createDataChannel(std::string(datagram_label), unreliable));
    } catch (const std::exception& error) {
        spdlog::warn("opening the link to {} failed: {}", format_address(peer->remote), error.what());
        mark_closed(peer);
    }
}

void Tunnel::attach_stream(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::weak_ptr<Peer> weak = peer;
    const auto opened = [this, weak] {
        const auto locked = weak.lock();
        if (!locked) {
            return;
        }
        std::scoped_lock lock(mutex_);
        if (locked->state != LinkState::connecting) {
            return;
        }
        locked->state = LinkState::open;
        spdlog::info("link to {} is open", format_address(locked->remote));
        if (!locked->offerer) {
            ready_.push_back(locked);
        }
    };

    {
        std::scoped_lock lock(mutex_);
        peer->stream = channel;
    }
    channel->onMessage([this, weak](rtc::message_variant message) {
        const auto* bytes = std::get_if<rtc::binary>(&message);
        const auto locked = weak.lock();
        if (bytes == nullptr || !locked) {
            return;
        }
        std::scoped_lock lock(mutex_);
        locked->inbox.push_back(to_bytes(*bytes));
    });
    channel->onClosed([this, weak] {
        if (const auto locked = weak.lock()) {
            mark_closed(locked);
        }
    });
    channel->onOpen(opened);
    if (channel->isOpen()) {
        opened();
    }
}

void Tunnel::attach_datagrams(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::weak_ptr<Peer> weak = peer;
    {
        std::scoped_lock lock(mutex_);
        peer->datagrams = channel;
    }
    channel->onMessage([this, weak](rtc::message_variant message) {
        const auto* bytes = std::get_if<rtc::binary>(&message);
        const auto locked = weak.lock();
        if (bytes == nullptr || !locked) {
            return;
        }
        Datagram datagram{locked->remote, to_bytes(*bytes)};
        {
            std::scoped_lock handler_lock(datagram_handler_mutex_);
            if (datagram_handler_) {
                datagram_handler_(datagram);
                return;
            }
        }
        std::scoped_lock lock(mutex_);
        datagrams_.push_back(std::move(datagram));
    });
}

void Tunnel::mark_closed(const std::shared_ptr<Peer>& peer) {
    std::scoped_lock lock(mutex_);
    if (peer->state == LinkState::closed) {
        return;
    }
    peer->state = LinkState::closed;
    spdlog::info("link to {} closed", format_address(peer->remote));
    std::erase(ready_, peer);
    const bool accepted = std::ranges::any_of(handles_, [&peer](const auto& entry) { return entry.second == peer; });
    if (!accepted && std::erase(incoming_, peer) != 0) {
        released_.push_back(peer);
        wake_.notify_all();
    }
}

void Tunnel::start_accepting() {
    std::scoped_lock lock(mutex_);
    accepting_ = true;
    wake_.notify_all();
}

void Tunnel::stop_accepting() {
    std::vector<std::shared_ptr<Peer>> closing;
    {
        std::scoped_lock lock(mutex_);
        accepting_ = false;
        closing = std::exchange(incoming_, {});
        ready_.clear();
        std::erase_if(handles_, [](const auto& entry) { return !entry.second->offerer; });
    }
    spdlog::info("game stopped accepting, closing {} incoming links", closing.size());
    close_all(closing);
}

bool Tunnel::has_ready() const {
    std::scoped_lock lock(mutex_);
    return !ready_.empty();
}

bool Tunnel::accept_into(int handle) {
    std::scoped_lock lock(mutex_);
    if (ready_.empty()) {
        return false;
    }
    const auto peer = ready_.front();
    ready_.pop_front();
    handles_[handle] = peer;
    spdlog::info("accepted {} as {}", format_address(peer->remote), handle);
    return true;
}

void Tunnel::connect(int handle, std::uint32_t remote) {
    close(handle);

    auto peer = std::make_shared<Peer>();
    peer->remote = remote;
    peer->offerer = true;
    spdlog::info("connecting to {} as {}", format_address(remote), handle);

    std::scoped_lock lock(mutex_);
    peer->link = std::format("{:016x}", link_ids_());
    handles_[handle] = peer;
    unopened_.push_back(peer);
    wake_.notify_all();
}

bool Tunnel::owns(int handle) const {
    std::scoped_lock lock(mutex_);
    return handles_.contains(handle);
}

std::optional<std::uint32_t> Tunnel::remote(int handle) const {
    std::scoped_lock lock(mutex_);
    const auto found = handles_.find(handle);
    if (found == handles_.end()) {
        return std::nullopt;
    }
    return found->second->remote;
}

std::optional<std::string> Tunnel::link(int handle) const {
    std::scoped_lock lock(mutex_);
    const auto found = handles_.find(handle);
    if (found == handles_.end()) {
        return std::nullopt;
    }
    return found->second->link;
}

LinkState Tunnel::state(int handle) const {
    std::scoped_lock lock(mutex_);
    const auto found = handles_.find(handle);
    return found == handles_.end() ? LinkState::closed : found->second->state;
}

bool Tunnel::send(int handle, std::span<const std::uint8_t> frame) {
    std::shared_ptr<rtc::DataChannel> stream;
    {
        std::scoped_lock lock(mutex_);
        const auto found = handles_.find(handle);
        if (found == handles_.end()) {
            return false;
        }
        stream = found->second->stream;
    }
    return stream && stream->isOpen() && send_binary(*stream, frame);
}

Received Tunnel::receive(int handle) {
    std::scoped_lock lock(mutex_);
    const auto found = handles_.find(handle);
    if (found == handles_.end()) {
        return Received{Received::Status::closed, {}};
    }
    return take_frame(*found->second);
}

void Tunnel::close(int handle) {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        const auto found = handles_.find(handle);
        if (found == handles_.end()) {
            return;
        }
        peer = found->second;
        handles_.erase(found);
        std::erase(incoming_, peer);
        std::erase(unopened_, peer);
    }
    spdlog::info("game closed {} to {}", handle, format_address(peer->remote));
    close_all({peer});
}

bool Tunnel::send_datagram(std::uint32_t to, std::span<const std::uint8_t> datagram) {
    std::shared_ptr<rtc::DataChannel> channel;
    bool linked = false;
    {
        std::scoped_lock lock(mutex_);
        const auto consider = [&](const std::shared_ptr<Peer>& peer) {
            if (peer->remote != to) {
                return;
            }
            linked = true;
            if (peer->datagrams && peer->datagrams->isOpen()) {
                channel = peer->datagrams;
            }
        };
        for (const auto& [handle, peer] : handles_) {
            consider(peer);
        }
        for (const auto& peer : incoming_) {
            consider(peer);
        }
    }
    if (!channel) {
        if (std::has_single_bit(++dropped_datagrams_)) {
            spdlog::warn("datagram to {} dropped: {} ({} dropped so far)", format_address(to), linked ? "channel not open" : "no link to that address", dropped_datagrams_.load());
        }
        return false;
    }
    return send_binary(*channel, datagram);
}

std::optional<Datagram> Tunnel::receive_datagram() {
    std::scoped_lock lock(mutex_);
    if (datagrams_.empty()) {
        return std::nullopt;
    }
    auto datagram = std::move(datagrams_.front());
    datagrams_.pop_front();
    return datagram;
}

void Tunnel::on_datagram(DatagramHandler handler) {
    std::scoped_lock lock(datagram_handler_mutex_);
    datagram_handler_ = std::move(handler);
}

void Tunnel::answer(std::uint32_t remote, const std::string& link, const std::string& sdp) {
    {
        std::scoped_lock lock(mutex_);
        if (!accepting_) {
            return;
        }
    }

    auto peer = std::make_shared<Peer>();
    peer->remote = remote;
    peer->link = link;
    const auto connection = open_connection(peer, peer_configuration(signaling_.ice_servers()));
    const std::weak_ptr<Peer> weak = peer;
    connection->onDataChannel([this, weak](std::shared_ptr<rtc::DataChannel> channel) {
        const auto locked = weak.lock();
        if (!locked) {
            return;
        }
        if (channel->label() == stream_label) {
            attach_stream(locked, channel);
        } else if (channel->label() == datagram_label) {
            attach_datagrams(locked, channel);
        }
    });

    {
        std::scoped_lock lock(mutex_);
        if (accepting_) {
            incoming_.push_back(peer);
        } else {
            peer.reset();
        }
    }
    if (!peer) {
        connection->close();
        return;
    }
    spdlog::info("answering {}", format_address(remote));
    connection->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));
}

void Tunnel::accept_answer(std::uint32_t remote, const std::string& link, const std::string& sdp) {
    std::shared_ptr<rtc::PeerConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        const auto peer = outgoing_link(remote, link);
        if (peer && peer->connection && !peer->answered) {
            peer->answered = true;
            connection = peer->connection;
        }
    }
    if (!connection) {
        return;
    }
    spdlog::info("answer from {}", format_address(remote));
    connection->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
}

void Tunnel::handle(const Message& message) {
    const auto remote = parse_address(message.from);
    if (!remote || message.link.empty()) {
        return;
    }

    switch (message.type) {
    case MessageType::offer:
        answer(*remote, message.link, message.sdp);
        break;
    case MessageType::answer:
        accept_answer(*remote, message.link, message.sdp);
        break;
    case MessageType::candidate:
        add_remote_candidate(*remote, message);
        break;
    }
}

std::shared_ptr<Tunnel::Peer> Tunnel::incoming_link(std::uint32_t remote, const std::string& link) const {
    const auto found = std::ranges::find_if(incoming_, [&](const auto& peer) { return peer->remote == remote && peer->link == link; });
    return found == incoming_.end() ? nullptr : *found;
}

std::shared_ptr<Tunnel::Peer> Tunnel::outgoing_link(std::uint32_t remote, const std::string& link) const {
    const auto found = std::ranges::find_if(handles_, [&](const auto& entry) {
        const auto& peer = entry.second;
        return peer->offerer && peer->remote == remote && peer->link == link;
    });
    return found == handles_.end() ? nullptr : found->second;
}

void Tunnel::add_remote_candidate(std::uint32_t remote, const Message& message) {
    std::shared_ptr<rtc::PeerConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        std::shared_ptr<Peer> peer;
        if (message.from_offerer != false) {
            peer = incoming_link(remote, message.link);
        }
        if (!peer && message.from_offerer != true) {
            peer = outgoing_link(remote, message.link);
            if (peer && !peer->answered) {
                peer.reset();
            }
        }
        if (peer) {
            connection = peer->connection;
        }
    }
    if (connection) {
        connection->addRemoteCandidate(rtc::Candidate(message.candidate, message.mid));
    }
}

Received Tunnel::take_frame(Peer& peer) {
    if (!peer.inbox.empty()) {
        auto frame = std::move(peer.inbox.front());
        peer.inbox.pop_front();
        return Received{Received::Status::frame, std::move(frame)};
    }
    if (peer.state == LinkState::closed) {
        return Received{Received::Status::closed, {}};
    }
    return Received{Received::Status::empty, {}};
}

void Tunnel::close_all(const std::vector<std::shared_ptr<Peer>>& peers) {
    std::vector<std::shared_ptr<rtc::PeerConnection>> connections;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& peer : peers) {
            if (peer->connection) {
                connections.push_back(peer->connection);
            }
        }
    }
    for (const auto& connection : connections) {
        connection->close();
    }
}

void Tunnel::run(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        if (!wake_.wait(lock, stop, [this] { return !inbox_.empty() || !unopened_.empty() || !released_.empty(); })) {
            break;
        }

        auto messages = std::exchange(inbox_, {});
        auto opening = std::exchange(unopened_, {});
        const std::vector released(released_.begin(), released_.end());
        released_.clear();
        lock.unlock();

        close_all(released);
        for (const auto& peer : opening) {
            open_outgoing(peer);
        }
        for (const auto& message : messages) {
            try {
                handle(message);
            } catch (const std::exception& error) {
                spdlog::warn("signaling message from {} failed: {}", message.from, error.what());
            }
        }

        lock.lock();
    }
}

}
