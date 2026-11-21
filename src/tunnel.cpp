#include "tunnel.h"

#include "ipv4.h"

#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cstring>
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
        return channel.send(to_binary(bytes));
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
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> stream;
    std::shared_ptr<rtc::DataChannel> datagrams;
    std::deque<std::vector<std::uint8_t>> inbox;
    LinkState state = LinkState::connecting;
};

Tunnel::Tunnel(SignalingLink& signaling) : signaling_(signaling), thread_([this](std::stop_token stop) { run(stop); }) {
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
        peers = std::exchange(host_peers_, {});
        for (const auto& [socket, peer] : sockets_) {
            peers.push_back(peer);
        }
        if (client_) {
            peers.push_back(client_);
        }
        sockets_.clear();
        ready_.clear();
        client_.reset();
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
        if (signaling_.send(offer ? MessageType::offer : MessageType::answer, to, std::string(description))) {
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
        if (!signaling_.send_candidate(format_address(locked->remote), std::string(candidate), candidate.mid())) {
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

void Tunnel::open_client() {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        if (!client_waiting_for_connection()) {
            return;
        }
        peer = client_;
    }

    try {
        const auto connection = open_connection(peer, peer_configuration(signaling_.ice_servers()));
        {
            std::scoped_lock lock(mutex_);
            if (client_ != peer) {
                peer.reset();
            }
        }
        if (!peer) {
            connection->close();
            return;
        }

        rtc::DataChannelInit unreliable;
        unreliable.reliability.unordered = true;
        unreliable.reliability.maxRetransmits = 0;
        attach_stream(peer, connection->createDataChannel(std::string(stream_label)), false);
        attach_datagrams(peer, connection->createDataChannel(std::string(datagram_label), unreliable));
    } catch (const std::exception& error) {
        spdlog::warn("opening the link to {} failed: {}", format_address(peer->remote), error.what());
        mark_closed(peer);
    }
}

bool Tunnel::client_waiting_for_connection() const {
    return client_ && !client_->connection && client_->state != LinkState::closed;
}

void Tunnel::attach_stream(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel, bool accepted_by_host) {
    const std::weak_ptr<Peer> weak = peer;
    const auto opened = [this, weak, accepted_by_host] {
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
        if (accepted_by_host) {
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
        std::scoped_lock lock(mutex_);
        datagrams_.push_back(Datagram{locked->remote, to_bytes(*bytes)});
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
        closing = std::exchange(host_peers_, {});
        ready_.clear();
        sockets_.clear();
    }
    spdlog::info("game stopped accepting, closing {} host links", closing.size());
    close_all(std::move(closing));
}

bool Tunnel::accept_into(int socket) {
    std::scoped_lock lock(mutex_);
    if (ready_.empty()) {
        return false;
    }
    sockets_[socket] = ready_.front();
    ready_.pop_front();
    spdlog::info("accepted {} as socket {}", format_address(sockets_[socket]->remote), socket);
    return true;
}

bool Tunnel::owns_socket(int socket) const {
    std::scoped_lock lock(mutex_);
    return sockets_.contains(socket);
}

bool Tunnel::send_on_socket(int socket, std::span<const std::uint8_t> frame) {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        const auto found = sockets_.find(socket);
        if (found == sockets_.end()) {
            return false;
        }
        peer = found->second;
    }
    return send_stream(*peer, frame);
}

Received Tunnel::receive_on_socket(int socket) {
    std::scoped_lock lock(mutex_);
    const auto found = sockets_.find(socket);
    if (found == sockets_.end()) {
        return Received{Received::Status::closed, {}};
    }
    return take_frame(*found->second);
}

void Tunnel::close_socket(int socket) {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        const auto found = sockets_.find(socket);
        if (found == sockets_.end()) {
            return;
        }
        peer = found->second;
        sockets_.erase(found);
        std::erase(host_peers_, peer);
    }
    spdlog::info("game closed socket {} to {}", socket, format_address(peer->remote));
    close_all({peer});
}

void Tunnel::connect(std::uint32_t remote) {
    disconnect_client();

    auto peer = std::make_shared<Peer>();
    peer->remote = remote;
    spdlog::info("connecting to {}", format_address(remote));

    std::scoped_lock lock(mutex_);
    client_ = peer;
    client_answered_ = false;
    wake_.notify_all();
}

std::optional<std::uint32_t> Tunnel::client_remote() const {
    std::scoped_lock lock(mutex_);
    if (!client_) {
        return std::nullopt;
    }
    return client_->remote;
}

LinkState Tunnel::client_state() const {
    std::scoped_lock lock(mutex_);
    return client_ ? client_->state : LinkState::closed;
}

bool Tunnel::send_on_client(std::span<const std::uint8_t> frame) {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = client_;
    }
    return peer && send_stream(*peer, frame);
}

Received Tunnel::receive_on_client() {
    std::scoped_lock lock(mutex_);
    if (!client_) {
        return Received{Received::Status::closed, {}};
    }
    return take_frame(*client_);
}

void Tunnel::disconnect_client() {
    std::shared_ptr<Peer> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = std::exchange(client_, nullptr);
    }
    if (peer) {
        spdlog::info("game disconnected its client link to {}", format_address(peer->remote));
        close_all({peer});
    }
}

bool Tunnel::send_datagram(std::uint32_t to, std::span<const std::uint8_t> datagram) {
    std::shared_ptr<rtc::DataChannel> channel;
    {
        std::scoped_lock lock(mutex_);
        if (client_ && client_->remote == to) {
            channel = client_->datagrams;
        }
        for (const auto& [socket, peer] : sockets_) {
            if (peer->remote == to) {
                channel = peer->datagrams;
            }
        }
    }
    if (!channel || !channel->isOpen()) {
        if (std::has_single_bit(++dropped_datagrams_)) {
            spdlog::warn("datagram to {} dropped: {} ({} dropped so far)", format_address(to), channel ? "channel not open" : "no link to that address", dropped_datagrams_.load());
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

void Tunnel::answer(std::uint32_t remote, const std::string& sdp) {
    {
        std::scoped_lock lock(mutex_);
        if (!accepting_) {
            return;
        }
    }

    auto peer = std::make_shared<Peer>();
    peer->remote = remote;
    const auto connection = open_connection(peer, peer_configuration(signaling_.ice_servers()));
    const std::weak_ptr<Peer> weak = peer;
    connection->onDataChannel([this, weak](std::shared_ptr<rtc::DataChannel> channel) {
        const auto locked = weak.lock();
        if (!locked) {
            return;
        }
        if (channel->label() == stream_label) {
            attach_stream(locked, channel, true);
        } else if (channel->label() == datagram_label) {
            attach_datagrams(locked, channel);
        }
    });

    {
        std::scoped_lock lock(mutex_);
        if (accepting_) {
            host_peers_.push_back(peer);
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

void Tunnel::handle(const Message& message) {
    const auto remote = parse_address(message.from);
    if (!remote) {
        return;
    }

    if (message.type == MessageType::offer) {
        answer(*remote, message.sdp);
        return;
    }
    if (message.type == MessageType::candidate) {
        add_remote_candidate(*remote, message);
        return;
    }

    std::shared_ptr<rtc::PeerConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        if (!client_ || !client_->connection || client_->remote != *remote || client_answered_) {
            return;
        }
        client_answered_ = true;
        connection = client_->connection;
    }
    spdlog::info("answer from {}", message.from);
    connection->setRemoteDescription(rtc::Description(message.sdp, rtc::Description::Type::Answer));
}

void Tunnel::add_remote_candidate(std::uint32_t remote, const Message& message) {
    std::shared_ptr<rtc::PeerConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        if (client_ && client_->remote == remote && client_answered_) {
            connection = client_->connection;
        }
        for (const auto& peer : host_peers_) {
            if (peer->remote == remote) {
                connection = peer->connection;
            }
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

bool Tunnel::send_stream(const Peer& peer, std::span<const std::uint8_t> frame) {
    return peer.stream && peer.stream->isOpen() && send_binary(*peer.stream, frame);
}

void Tunnel::close_all(std::vector<std::shared_ptr<Peer>> peers) {
    for (const auto& peer : peers) {
        if (peer->connection) {
            peer->connection->close();
        }
    }
}

void Tunnel::run(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        if (!wake_.wait(lock, stop, [this] { return !inbox_.empty() || client_waiting_for_connection(); })) {
            break;
        }

        auto messages = std::exchange(inbox_, {});
        lock.unlock();

        open_client();
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
