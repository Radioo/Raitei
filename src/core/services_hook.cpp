#include "services_hook.h"

#include "module_scan.h"

#include <windows.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace raitei {

namespace {

constexpr std::wstring_view ea3_module = L"avs2-ea3.dll";
constexpr std::wstring_view core_module = L"avs2-core.dll";
constexpr std::string_view receiver_log_string = "services: no url.";
constexpr const char* property_node_refer_export = "XCgsqzn00000af";
constexpr const char* raitei_url_path = "raitei/url";
constexpr int property_type_str = 11;
constexpr std::size_t url_capacity = 512;

using PropertyNodeRefer = int (*)(void* property, void* node, const char* path, int type, void* out, std::uint32_t size);

safetyhook::InlineHook receiver_hook;
PropertyNodeRefer property_node_refer = nullptr;
SignalingClient* signaling = nullptr;

std::int64_t services_receiver(void* context, void* services) {
    std::array<char, url_capacity> url{};
    if (services != nullptr &&
        property_node_refer(nullptr, services, raitei_url_path, property_type_str, url.data(), static_cast<std::uint32_t>(url.size())) > 0) {
        spdlog::info("services.get sent signaling at {}", url.data());
        signaling->connect(url.data());
    } else {
        spdlog::warn("services.get has no <raitei><url>, online matching stays off");
    }
    return receiver_hook.call<std::int64_t>(context, services);
}

}

bool install_services_hook(SignalingClient& target_signaling) {
    const HMODULE ea3 = GetModuleHandleW(ea3_module.data());
    const HMODULE core = GetModuleHandleW(core_module.data());
    if (ea3 == nullptr || core == nullptr) {
        spdlog::error("avs2-ea3.dll or avs2-core.dll is not loaded");
        return false;
    }

    property_node_refer = reinterpret_cast<PropertyNodeRefer>(GetProcAddress(core, property_node_refer_export));
    if (property_node_refer == nullptr) {
        spdlog::error("property_node_refer ({}) not found in avs2-core.dll", property_node_refer_export);
        return false;
    }

    void* receiver = find_function_referencing(ea3, receiver_log_string);
    if (receiver == nullptr) {
        spdlog::error("services.get receiver not found in avs2-ea3.dll");
        return false;
    }

    auto created = safetyhook::InlineHook::create(receiver, reinterpret_cast<void*>(&services_receiver));
    if (!created) {
        spdlog::error("hooking the services.get receiver failed");
        return false;
    }
    receiver_hook = std::move(*created);
    signaling = &target_signaling;
    return true;
}

}
