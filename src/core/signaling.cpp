#include "signaling.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <limits>
#include <string_view>
#include <utility>
#include <variant>

namespace raitei {

namespace {

constexpr auto reconnect_delay = std::chrono::seconds{2};
constexpr auto connection_timeout = std::chrono::seconds{10};
constexpr auto ping_interval = std::chrono::seconds{15};
constexpr auto ice_timeout = std::chrono::seconds{8};
constexpr std::string_view hello_type = "hello";
constexpr std::string_view welcome_type = "welcome";
constexpr std::string_view offer_type = "offer";
constexpr std::string_view answer_type = "answer";
constexpr std::string_view ice_type = "ice";
constexpr std::string_view candidate_type = "candidate";
constexpr const char* offerer_field = "offerer";
constexpr const char* link_field = "link";

std::string string_field(const nlohmann::json& frame, const char* name) {
    const auto found = frame.find(name);
    return found != frame.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::uint16_t port_field(const nlohmann::json& frame, const char* name) {
    const auto found = frame.find(name);
    if (found == frame.end() || !found->is_number_unsigned()) {
        return 0;
    }
    const auto value = found->get<std::uint64_t>();
    return value <= std::numeric_limits<std::uint16_t>::max() ? static_cast<std::uint16_t>(value) : 0;
}

std::optional<bool> bool_field(const nlohmann::json& frame, const char* name) {
    const auto found = frame.find(name);
    if (found == frame.end() || !found->is_boolean()) {
        return std::nullopt;
    }
    return found->get<bool>();
}

std::string_view type_name(MessageType type) {
    return type == MessageType::offer ? offer_type : answer_type;
}

std::vector<IceServer> parse_ice_servers(const nlohmann::json& list) {
    std::vector<IceServer> servers;
    for (const auto& item : list) {
        if (!item.is_object() || !item.contains("urls") || !item["urls"].is_array()) {
            continue;
        }
        IceServer server;
        for (const auto& url : item["urls"]) {
            if (url.is_string()) {
                server.urls.push_back(url.get<std::string>());
            }
        }
        server.username = string_field(item, "username");
        server.credential = string_field(item, "credential");
        servers.push_back(std::move(server));
    }
    return servers;
}

}

SignalingClient::SignalingClient() : thread_([this](std::stop_token stop) { run(stop); }) {
}

SignalingClient::~SignalingClient() {
    thread_.request_stop();
    thread_.join();
    std::shared_ptr<rtc::WebSocket> socket;
    {
        std::scoped_lock lock(mutex_);
        socket = std::exchange(socket_, nullptr);
        ++generation_;
    }
    if (socket) {
        socket->resetCallbacks();
        socket->close();
    }
}

void SignalingClient::connect(std::string url) {
    std::scoped_lock lock(mutex_);
    if (url == url_ && socket_) {
        return;
    }
    url_ = std::move(url);
    reconnect_ = true;
    retrying_ = false;
    wake_.notify_all();
}

void SignalingClient::on_message(MessageHandler handler) {
    std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
}

bool SignalingClient::configured() const {
    std::scoped_lock lock(mutex_);
    return !url_.empty();
}

std::optional<Registration> SignalingClient::registration() const {
    std::scoped_lock lock(mutex_);
    return registration_;
}

bool SignalingClient::send(MessageType type, const std::string& to, const std::string& link, const std::string& sdp) {
    return send_frame({{"type", type_name(type)}, {"to", to}, {link_field, link}, {"sdp", sdp}});
}

bool SignalingClient::send_candidate(const std::string& to, const std::string& link, const std::string& candidate, const std::string& mid, bool from_offerer) {
    return send_frame({{"type", candidate_type}, {"to", to}, {link_field, link}, {"candidate", candidate}, {"mid", mid}, {offerer_field, from_offerer}});
}

std::optional<std::vector<IceServer>> SignalingClient::ice_servers() {
    std::scoped_lock call(ice_call_);
    {
        std::scoped_lock lock(mutex_);
        ice_reply_.reset();
    }
    if (!send_frame({{"type", ice_type}})) {
        spdlog::warn("ice servers unavailable: signaling is not connected");
        return std::nullopt;
    }

    std::unique_lock lock(mutex_);
    if (!ice_wake_.wait_for(lock, ice_timeout, [this] { return ice_reply_.has_value(); })) {
        spdlog::warn("ice servers unavailable: no reply from signaling");
        return std::nullopt;
    }
    return std::move(*ice_reply_);
}

bool SignalingClient::send_frame(const nlohmann::json& frame) {
    std::shared_ptr<rtc::WebSocket> socket;
    {
        std::scoped_lock lock(mutex_);
        socket = socket_;
    }
    if (!socket || !socket->isOpen()) {
        return false;
    }
    try {
        return socket->send(frame.dump());
    } catch (const std::exception& error) {
        spdlog::warn("signaling send failed: {}", error.what());
        return false;
    }
}

void SignalingClient::run(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        if (!wake_.wait(lock, stop, [this] { return reconnect_; })) {
            break;
        }
        if (std::exchange(retrying_, false)) {
            wake_.wait_for(lock, stop, reconnect_delay, [] { return false; });
            if (stop.stop_requested()) {
                break;
            }
        }
        reconnect_ = false;
        const auto generation = ++generation_;
        const auto url = url_;
        auto socket = create_socket(generation);
        auto previous = std::exchange(socket_, socket);
        lock.unlock();

        if (previous) {
            previous->close();
        }
        try {
            socket->open(url);
        } catch (const std::exception& error) {
            spdlog::error("cannot open signaling at {}: {}", url, error.what());
            lost(generation);
        }

        lock.lock();
    }
}

std::shared_ptr<rtc::WebSocket> SignalingClient::create_socket(std::uint64_t generation) {
    rtc::WebSocketConfiguration configuration;
    configuration.connectionTimeout = connection_timeout;
    configuration.pingInterval = ping_interval;
    auto socket = std::make_shared<rtc::WebSocket>(configuration);

    socket->onOpen([this, generation] {
        nlohmann::json hello{{"type", hello_type}};
        std::shared_ptr<rtc::WebSocket> current;
        {
            std::scoped_lock lock(mutex_);
            if (generation != generation_ || !socket_) {
                return;
            }
            current = socket_;
            if (registration_) {
                hello["address"] = registration_->address;
                hello["token"] = registration_->token;
            }
        }
        spdlog::info("signaling connected");
        current->send(hello.dump());
    });
    socket->onMessage([this](rtc::message_variant message) {
        if (const auto* text = std::get_if<std::string>(&message)) {
            receive(*text);
        }
    });
    socket->onError([](std::string error) {
        spdlog::warn("signaling error: {}", error);
    });
    socket->onClosed([this, generation] { lost(generation); });
    return socket;
}

void SignalingClient::lost(std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    if (generation != generation_) {
        return;
    }
    spdlog::warn("signaling connection lost, reconnecting");
    reconnect_ = true;
    retrying_ = true;
    wake_.notify_all();
}

void SignalingClient::receive(const std::string& text) {
    const auto frame = nlohmann::json::parse(text, nullptr, false);
    if (frame.is_discarded() || !frame.is_object() || !frame.contains("type") || !frame["type"].is_string()) {
        return;
    }
    const auto type = frame["type"].get<std::string>();

    if (type == welcome_type) {
        Registration registration{
            .address = string_field(frame, "address"),
            .token = string_field(frame, "token"),
            .port = port_field(frame, "port"),
        };
        if (registration.address.empty() || registration.token.empty()) {
            spdlog::warn("ignoring a welcome without an address and token");
            return;
        }
        std::scoped_lock lock(mutex_);
        if (!registration_ || registration_->address != registration.address) {
            spdlog::info("registered as {}", registration.address);
        }
        registration_ = std::move(registration);
        return;
    }

    if (type == ice_type) {
        std::optional<std::vector<IceServer>> servers;
        if (frame.contains("servers") && frame["servers"].is_array()) {
            servers = parse_ice_servers(frame["servers"]);
        } else {
            const auto reason = string_field(frame, "error");
            spdlog::warn("ice servers unavailable: {}", reason.empty() ? "no reason given" : reason);
        }
        std::scoped_lock lock(mutex_);
        ice_reply_ = std::move(servers);
        ice_wake_.notify_all();
        return;
    }

    Message message{.type = MessageType::offer, .from = string_field(frame, "from"), .link = string_field(frame, link_field)};
    if (type == offer_type || type == answer_type) {
        message.type = type == offer_type ? MessageType::offer : MessageType::answer;
        message.sdp = string_field(frame, "sdp");
    } else if (type == candidate_type) {
        message.type = MessageType::candidate;
        message.candidate = string_field(frame, "candidate");
        message.mid = string_field(frame, "mid");
        message.from_offerer = bool_field(frame, offerer_field);
    } else {
        return;
    }
    MessageHandler handler;
    {
        std::scoped_lock lock(mutex_);
        handler = handler_;
    }
    if (handler) {
        handler(std::move(message));
    }
}

}
