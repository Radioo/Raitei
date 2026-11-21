#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rtc {
class WebSocket;
}

namespace raitei {

struct Registration {
    std::string address;
    std::string token;
    std::uint16_t port = 0;
};

enum class MessageType {
    offer,
    answer,
    candidate,
};

struct IceServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct Message {
    MessageType type;
    std::string from;
    std::string sdp;
    std::string candidate;
    std::string mid;
};

class SignalingLink {
public:
    using MessageHandler = std::function<void(Message)>;

    SignalingLink() = default;
    SignalingLink(const SignalingLink&) = delete;
    SignalingLink& operator=(const SignalingLink&) = delete;
    virtual ~SignalingLink() = default;

    virtual void on_message(MessageHandler handler) = 0;
    virtual bool send(MessageType type, const std::string& to, const std::string& sdp) = 0;
    virtual bool send_candidate(const std::string& to, const std::string& candidate, const std::string& mid) = 0;
    virtual std::optional<std::vector<IceServer>> ice_servers() = 0;
};

class SignalingClient final : public SignalingLink {
public:
    SignalingClient();
    ~SignalingClient() override;

    void connect(std::string url);
    void on_message(MessageHandler handler) override;
    std::optional<Registration> registration() const;
    bool send(MessageType type, const std::string& to, const std::string& sdp) override;
    bool send_candidate(const std::string& to, const std::string& candidate, const std::string& mid) override;
    std::optional<std::vector<IceServer>> ice_servers() override;

private:
    void run(std::stop_token stop);
    std::shared_ptr<rtc::WebSocket> create_socket(std::uint64_t generation);
    void lost(std::uint64_t generation);
    void receive(const std::string& text);
    bool send_frame(const nlohmann::json& frame);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::string url_;
    bool reconnect_ = false;
    bool retrying_ = false;
    std::uint64_t generation_ = 0;
    std::shared_ptr<rtc::WebSocket> socket_;
    std::optional<Registration> registration_;
    MessageHandler handler_;

    std::mutex ice_call_;
    std::condition_variable ice_wake_;
    std::optional<std::optional<std::vector<IceServer>>> ice_reply_;

    std::jthread thread_;
};

}
