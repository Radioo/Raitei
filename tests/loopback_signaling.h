#pragma once

#include "signaling.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace raitei::testing {

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds{10});

class LoopbackSignaling {
public:
    class Endpoint final : public SignalingLink {
    public:
        Endpoint(LoopbackSignaling& hub, std::string address, std::vector<IceServer> servers);

        void on_message(MessageHandler handler) override;
        bool send(MessageType type, const std::string& to, const std::string& sdp) override;
        bool send_candidate(const std::string& to, const std::string& candidate, const std::string& mid) override;
        std::optional<std::vector<IceServer>> ice_servers() override;

        void deliver(Message message);
        const std::string& address() const;

    private:
        LoopbackSignaling& hub_;
        std::string address_;
        std::vector<IceServer> servers_;
        std::mutex mutex_;
        MessageHandler handler_;
    };

    Endpoint& join(const std::string& address, std::vector<IceServer> servers = {});
    void inject(const std::string& to, Message message);

private:
    bool route(const std::string& to, Message message);

    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Endpoint>> endpoints_;
};

}
