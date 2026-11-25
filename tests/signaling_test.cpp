#include "loopback_signaling.h"
#include "core/signaling.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <chrono>
#include <format>
#include <mutex>
#include <vector>

using namespace raitei;
using raitei::testing::eventually;

namespace {

constexpr auto reconnect_wait = std::chrono::seconds{8};
constexpr auto prompt_reply = std::chrono::seconds{2};

class SwitchboardStandIn {
public:
    SwitchboardStandIn() : server_(configuration()) {
        server_.onClient([this](std::shared_ptr<rtc::WebSocket> client) {
            client->onMessage([this](rtc::message_variant message) {
                if (const auto* text = std::get_if<std::string>(&message)) {
                    received(nlohmann::json::parse(*text));
                }
            });
            std::scoped_lock lock(mutex_);
            client_ = std::move(client);
        });
    }

    SwitchboardStandIn(const SwitchboardStandIn&) = delete;
    SwitchboardStandIn& operator=(const SwitchboardStandIn&) = delete;

    ~SwitchboardStandIn() {
        server_.stop();
        if (const auto client = current()) {
            client->resetCallbacks();
            client->close();
        }
    }

    std::string url() const {
        return std::format("ws://127.0.0.1:{}/api/raitei/v1/socket", server_.port());
    }

    void reply(std::function<std::optional<nlohmann::json>(const nlohmann::json&)> answer) {
        std::scoped_lock lock(mutex_);
        answer_ = std::move(answer);
    }

    bool send(const std::string& text) {
        const auto client = current();
        return client && client->isOpen() && client->send(text);
    }

    bool send_binary(const rtc::binary& bytes) {
        const auto client = current();
        return client && client->isOpen() && client->send(bytes);
    }

    void drop_client() {
        if (const auto client = current()) {
            client->close();
        }
    }

    std::vector<nlohmann::json> hellos() {
        std::scoped_lock lock(mutex_);
        return hellos_;
    }

private:
    static rtc::WebSocketServerConfiguration configuration() {
        rtc::WebSocketServerConfiguration configuration;
        configuration.port = 0;
        configuration.bindAddress = "127.0.0.1";
        return configuration;
    }

    std::shared_ptr<rtc::WebSocket> current() {
        std::scoped_lock lock(mutex_);
        return client_;
    }

    void received(const nlohmann::json& frame) {
        std::function<std::optional<nlohmann::json>(const nlohmann::json&)> answer;
        {
            std::scoped_lock lock(mutex_);
            if (frame.value("type", "") == "hello") {
                hellos_.push_back(frame);
            }
            answer = answer_;
        }
        if (!answer) {
            return;
        }
        if (const auto response = answer(frame)) {
            send(response->dump());
        }
    }

    std::mutex mutex_;
    std::shared_ptr<rtc::WebSocket> client_;
    std::vector<nlohmann::json> hellos_;
    std::function<std::optional<nlohmann::json>(const nlohmann::json&)> answer_;
    rtc::WebSocketServer server_;
};

std::optional<nlohmann::json> welcome(const nlohmann::json& frame) {
    if (frame.value("type", "") != "hello") {
        return std::nullopt;
    }
    return nlohmann::json{{"type", "welcome"}, {"address", "100.64.0.20"}, {"token", "secret-token"}, {"port", 5700}};
}

bool registered(const SignalingClient& client) {
    const auto registration = client.registration();
    return registration && registration->address == "100.64.0.20";
}

}

TEST_CASE("the client says hello and registers from the welcome") {
    SwitchboardStandIn switchboard;
    switchboard.reply(welcome);
    SignalingClient client;
    client.connect(switchboard.url());

    REQUIRE(eventually([&] { return registered(client); }));
    const auto registration = *client.registration();
    CHECK(registration.token == "secret-token");
    CHECK(registration.port == 5700);
    REQUIRE(switchboard.hellos().size() == 1);
    CHECK_FALSE(switchboard.hellos().front().contains("address"));
}

TEST_CASE("after losing the connection the client reconnects and asks for its address back") {
    SwitchboardStandIn switchboard;
    switchboard.reply(welcome);
    SignalingClient client;
    client.connect(switchboard.url());
    REQUIRE(eventually([&] { return registered(client); }));

    switchboard.drop_client();

    REQUIRE(eventually([&] { return switchboard.hellos().size() == 2; }, reconnect_wait));
    const auto again = switchboard.hellos().back();
    CHECK(again.value("address", "") == "100.64.0.20");
    CHECK(again.value("token", "") == "secret-token");
}

TEST_CASE("offers, answers and candidates from the switchboard reach the handler") {
    SwitchboardStandIn switchboard;
    switchboard.reply(welcome);
    SignalingClient client;
    std::mutex mutex;
    std::vector<Message> messages;
    client.on_message([&](Message message) {
        std::scoped_lock lock(mutex);
        messages.push_back(std::move(message));
    });
    client.connect(switchboard.url());
    REQUIRE(eventually([&] { return registered(client); }));

    REQUIRE(switchboard.send(R"({"type":"offer","from":"100.64.0.5","link":"00000000000000d4","sdp":"v=0 offer"})"));
    REQUIRE(switchboard.send(R"({"type":"answer","from":"100.64.0.6","sdp":"v=0 answer"})"));
    REQUIRE(switchboard.send(R"({"type":"candidate","from":"100.64.0.6","candidate":"a=candidate:1 1 UDP 1 10.20.30.40 5000 typ host","mid":"0"})"));
    REQUIRE(switchboard.send(R"({"type":"candidate","from":"100.64.0.6","candidate":"c","mid":"0","offerer":true})"));
    REQUIRE(switchboard.send(R"({"type":"candidate","from":"100.64.0.6","candidate":"c","mid":"0","offerer":false})"));

    REQUIRE(eventually([&] {
        std::scoped_lock lock(mutex);
        return messages.size() == 5;
    }));
    std::scoped_lock lock(mutex);
    CHECK(messages[0].type == MessageType::offer);
    CHECK(messages[0].from == "100.64.0.5");
    CHECK(messages[0].sdp == "v=0 offer");
    CHECK(messages[0].link == "00000000000000d4");
    CHECK(messages[1].link.empty());
    CHECK(messages[1].type == MessageType::answer);
    CHECK(messages[1].from == "100.64.0.6");
    CHECK(messages[2].type == MessageType::candidate);
    CHECK(messages[2].candidate == "a=candidate:1 1 UDP 1 10.20.30.40 5000 typ host");
    CHECK(messages[2].mid == "0");
    CHECK_FALSE(messages[2].from_offerer.has_value());
    CHECK(messages[3].from_offerer == true);
    CHECK(messages[4].from_offerer == false);
}

TEST_CASE("candidates say which side of the link sent them") {
    SwitchboardStandIn switchboard;
    std::mutex mutex;
    std::vector<nlohmann::json> candidates;
    switchboard.reply([&](const nlohmann::json& frame) -> std::optional<nlohmann::json> {
        if (frame.value("type", "") == "candidate") {
            std::scoped_lock lock(mutex);
            candidates.push_back(frame);
            return std::nullopt;
        }
        return welcome(frame);
    });
    SignalingClient client;
    CHECK_FALSE(client.configured());
    client.connect(switchboard.url());
    CHECK(client.configured());
    REQUIRE(eventually([&] { return registered(client); }));

    REQUIRE(client.send_candidate("100.64.0.5", "00000000000000a1", "a", "0", true));
    REQUIRE(client.send_candidate("100.64.0.5", "00000000000000b2", "b", "0", false));

    REQUIRE(eventually([&] {
        std::scoped_lock lock(mutex);
        return candidates.size() == 2;
    }));
    std::scoped_lock lock(mutex);
    CHECK(candidates[0].value("offerer", false) == true);
    CHECK(candidates[1].value("offerer", true) == false);
    CHECK(candidates[1].value("to", "") == "100.64.0.5");
    CHECK(candidates[0].value("link", "") == "00000000000000a1");
    CHECK(candidates[1].value("link", "") == "00000000000000b2");
}

TEST_CASE("frames the client cannot use are ignored without losing the connection") {
    SwitchboardStandIn switchboard;
    SignalingClient client;
    client.connect(switchboard.url());
    REQUIRE(eventually([&] { return !switchboard.hellos().empty(); }));

    CHECK(switchboard.send("not json"));
    CHECK(switchboard.send_binary(rtc::binary{std::byte{1}, std::byte{2}}));
    CHECK(switchboard.send(R"({"type":5})"));
    CHECK(switchboard.send(R"([1,2,3])"));
    CHECK(switchboard.send(R"({"type":"welcome","address":7,"token":"t","port":5700})"));
    CHECK(switchboard.send(R"({"type":"welcome","address":"100.64.0.20","token":"t","port":"5700"})"));
    CHECK(switchboard.send(R"({"type":"welcome","address":"100.64.0.20"})"));

    REQUIRE(eventually([&] { return client.registration().has_value(); }));
    CHECK(client.registration()->port == 0);
    CHECK(switchboard.hellos().size() == 1);
}

TEST_CASE("the ice reply becomes the server list") {
    SwitchboardStandIn switchboard;
    switchboard.reply([](const nlohmann::json& frame) -> std::optional<nlohmann::json> {
        if (frame.value("type", "") == "ice") {
            return nlohmann::json::parse(R"({"type":"ice","servers":[
                {"urls":["stun:stun.example.net:3478"]},
                {"urls":["turn:turn.example.net:3478?transport=udp", 9],"username":"user","credential":"pass"},
                {"urls":"not a list"},
                "not an object"]})");
        }
        return welcome(frame);
    });
    SignalingClient client;
    client.connect(switchboard.url());
    REQUIRE(eventually([&] { return registered(client); }));

    const auto servers = client.ice_servers();

    REQUIRE(servers);
    REQUIRE(servers->size() == 2);
    CHECK(servers->at(0).urls == std::vector<std::string>{"stun:stun.example.net:3478"});
    CHECK(servers->at(0).username.empty());
    CHECK(servers->at(1).urls == std::vector<std::string>{"turn:turn.example.net:3478?transport=udp"});
    CHECK(servers->at(1).username == "user");
    CHECK(servers->at(1).credential == "pass");
}

TEST_CASE("an ice error reply ends the wait at once") {
    SwitchboardStandIn switchboard;
    switchboard.reply([](const nlohmann::json& frame) -> std::optional<nlohmann::json> {
        if (frame.value("type", "") == "ice") {
            return nlohmann::json{{"type", "ice"}, {"error", "not matching"}};
        }
        return welcome(frame);
    });
    SignalingClient client;
    client.connect(switchboard.url());
    REQUIRE(eventually([&] { return registered(client); }));

    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(client.ice_servers());
    CHECK(std::chrono::steady_clock::now() - started < prompt_reply);
}

TEST_CASE("without a connection sends fail and there are no ice servers") {
    SignalingClient client;

    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(client.send(MessageType::offer, "100.64.0.5", "00000000000000c3", "v=0"));
    CHECK_FALSE(client.ice_servers());
    CHECK(std::chrono::steady_clock::now() - started < prompt_reply);
    CHECK_FALSE(client.registration());
}
