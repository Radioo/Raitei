#include "loopback_signaling.h"

#include <thread>

namespace raitei::testing {

namespace {

constexpr auto poll_interval = std::chrono::milliseconds{10};

}

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return condition();
}

LoopbackSignaling::Endpoint::Endpoint(LoopbackSignaling& hub, std::string address, std::vector<IceServer> servers)
    : hub_(hub), address_(std::move(address)), servers_(std::move(servers)) {
}

void LoopbackSignaling::Endpoint::on_message(MessageHandler handler) {
    std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
}

bool LoopbackSignaling::Endpoint::send(MessageType type, const std::string& to, const std::string& link, const std::string& sdp) {
    return hub_.route(to, Message{.type = type, .from = address_, .link = link, .sdp = sdp});
}

bool LoopbackSignaling::Endpoint::send_candidate(const std::string& to, const std::string& link, const std::string& candidate, const std::string& mid, bool from_offerer) {
    return hub_.route(to, Message{.type = MessageType::candidate, .from = address_, .link = link, .candidate = candidate, .mid = mid, .from_offerer = from_offerer});
}

std::optional<std::vector<IceServer>> LoopbackSignaling::Endpoint::ice_servers() {
    return servers_;
}

void LoopbackSignaling::Endpoint::deliver(Message message) {
    MessageHandler handler;
    {
        std::scoped_lock lock(mutex_);
        handler = handler_;
    }
    if (handler) {
        handler(std::move(message));
    }
}

const std::string& LoopbackSignaling::Endpoint::address() const {
    return address_;
}

LoopbackSignaling::Endpoint& LoopbackSignaling::join(const std::string& address, std::vector<IceServer> servers) {
    std::scoped_lock lock(mutex_);
    auto& endpoint = endpoints_[address];
    endpoint = std::make_unique<Endpoint>(*this, address, std::move(servers));
    return *endpoint;
}

void LoopbackSignaling::inject(const std::string& to, Message message) {
    Endpoint* endpoint = nullptr;
    {
        std::scoped_lock lock(mutex_);
        const auto found = endpoints_.find(to);
        if (found == endpoints_.end()) {
            return;
        }
        endpoint = found->second.get();
    }
    endpoint->deliver(std::move(message));
}

std::vector<Message> LoopbackSignaling::delivered_to(const std::string& address) {
    std::scoped_lock lock(mutex_);
    return delivered_[address];
}

bool LoopbackSignaling::route(const std::string& to, Message message) {
    {
        std::scoped_lock lock(mutex_);
        if (!endpoints_.contains(to)) {
            return false;
        }
        delivered_[to].push_back(message);
    }
    inject(to, std::move(message));
    return true;
}

}
