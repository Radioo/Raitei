#pragma once

#include "signaling.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

class Tunnel {
public:
    explicit Tunnel(SignalingLink& signaling);
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;
    ~Tunnel();

    void start_accepting();
    void stop_accepting();
    bool accept_into(int socket);
    bool owns_socket(int socket) const;
    bool send_on_socket(int socket, std::span<const std::uint8_t> frame);
    Received receive_on_socket(int socket);
    void close_socket(int socket);

    void connect(std::uint32_t remote);
    std::optional<std::uint32_t> client_remote() const;
    LinkState client_state() const;
    bool send_on_client(std::span<const std::uint8_t> frame);
    Received receive_on_client();
    void disconnect_client();

    bool send_datagram(std::uint32_t to, std::span<const std::uint8_t> datagram);
    std::optional<Datagram> receive_datagram();

private:
    struct Peer;

    std::shared_ptr<rtc::PeerConnection> open_connection(const std::shared_ptr<Peer>& peer, const rtc::Configuration& configuration);
    void open_client();
    bool client_waiting_for_connection() const;
    void attach_stream(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel, bool accepted_by_host);
    void attach_datagrams(const std::shared_ptr<Peer>& peer, const std::shared_ptr<rtc::DataChannel>& channel);
    void mark_closed(const std::shared_ptr<Peer>& peer);
    void answer(std::uint32_t remote, const std::string& sdp);
    void handle(const Message& message);
    void add_remote_candidate(std::uint32_t remote, const Message& message);
    static Received take_frame(Peer& peer);
    static bool send_stream(const Peer& peer, std::span<const std::uint8_t> frame);
    static void close_all(std::vector<std::shared_ptr<Peer>> peers);
    void run(std::stop_token stop);

    SignalingLink& signaling_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    bool accepting_ = false;
    std::vector<std::shared_ptr<Peer>> host_peers_;
    std::deque<std::shared_ptr<Peer>> ready_;
    std::map<int, std::shared_ptr<Peer>> sockets_;
    std::shared_ptr<Peer> client_;
    bool client_answered_ = false;
    std::deque<Datagram> datagrams_;
    std::deque<Message> inbox_;
    std::atomic<std::uint64_t> dropped_datagrams_{0};
    std::jthread thread_;
};

}
