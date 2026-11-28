#pragma once

#include "signaling.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace rtc {
class DataChannel;
class PeerConnection;
struct Configuration;
}

namespace raitei {

enum class LinkState {
    connecting,
    open,
    closed,
};

struct Received {
    enum class Status {
        frame,
        empty,
        closed,
    };

    Status status;
    std::vector<std::uint8_t> bytes;
};

struct Datagram {
    std::uint32_t from;
    std::vector<std::uint8_t> bytes;
};

using DatagramHandler = std::function<void(const Datagram&)>;

class Tunnel {
public:
    explicit Tunnel(SignalingLink& signaling);
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;
    ~Tunnel();

    void start_accepting();
    void stop_accepting();
    bool has_ready() const;
    bool accept_into(int handle);

    void connect(int handle, std::uint32_t remote);
    bool owns(int handle) const;
    std::optional<std::uint32_t> remote(int handle) const;
    std::optional<std::string> link(int handle) const;
    LinkState state(int handle) const;
    bool send(int handle, std::span<const std::uint8_t> frame);
    Received receive(int handle);
    void close(int handle);

    bool send_datagram(std::uint32_t to, std::span<const std::uint8_t> datagram);
    std::optional<Datagram> receive_datagram();
    void on_datagram(DatagramHandler handler);

private:
    struct Peer;

    std::shared_ptr<rtc::PeerConnection> open_connection(const std::shared_ptr<Peer>& peer, const rtc::Configuration& configuration);
    void open_outgoing(const std::shared_ptr<Peer>& peer);
    void attach_stream(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel);
    void attach_datagrams(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel);
    void mark_closed(const std::shared_ptr<Peer>& peer);
    void answer(std::uint32_t remote, const std::string& link, const std::string& sdp);
    void accept_answer(std::uint32_t remote, const std::string& link, const std::string& sdp);
    void handle(const Message& message);
    void add_remote_candidate(std::uint32_t remote, const Message& message);
    std::shared_ptr<Peer> incoming_link(std::uint32_t remote, const std::string& link) const;
    std::shared_ptr<Peer> outgoing_link(std::uint32_t remote, const std::string& link) const;
    static Received take_frame(Peer& peer);
    void close_all(const std::vector<std::shared_ptr<Peer>>& peers);
    void run(std::stop_token stop);

    SignalingLink& signaling_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    bool accepting_ = false;
    std::vector<std::shared_ptr<Peer>> incoming_;
    std::deque<std::shared_ptr<Peer>> ready_;
    std::map<int, std::shared_ptr<Peer>> handles_;
    std::deque<std::shared_ptr<Peer>> unopened_;
    std::deque<std::shared_ptr<Peer>> released_;
    std::mt19937_64 link_ids_;
    std::deque<Datagram> datagrams_;
    std::mutex datagram_handler_mutex_;
    DatagramHandler datagram_handler_;
    std::deque<Message> inbox_;
    std::atomic<std::uint64_t> dropped_datagrams_{0};
    std::jthread thread_;
};

}
