#include "core/services_hook.h"
#include "core/signaling.h"
#include "core/tunnel.h"
#include "games/game.h"

#include <windows.h>

#include <rtc/rtc.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view log_stem = "raitei";
constexpr const char* instance_variable = "SPICE2X_INSTANCE";
constexpr std::size_t instance_name_capacity = 256;

struct Runtime {
    Runtime() : tunnel(signaling) {
    }

    raitei::SignalingClient signaling;
    raitei::Tunnel tunnel;
};

std::string log_file_name() {
    std::array<char, instance_name_capacity> instance{};
    const auto length = GetEnvironmentVariableA(instance_variable, instance.data(), static_cast<DWORD>(instance.size()));
    if (length == 0 || length >= instance.size()) {
        return std::format("{}.log", log_stem);
    }
    return std::format("{}-{}.log", log_stem, std::string_view(instance.data(), length));
}

std::filesystem::path module_directory(HMODULE module) {
    std::array<wchar_t, MAX_PATH> path{};
    GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(path.data()).parent_path();
}

void start(HMODULE module) {
    auto logger = spdlog::basic_logger_mt("raitei", (module_directory(module) / log_file_name()).string(), true);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
    spdlog::info("Raitei {}", RAITEI_VERSION);

    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, std::string message) {
        spdlog::warn("rtc: {}", message);
    });

    auto* runtime = new Runtime();
    if (!raitei::install_services_hook(runtime->signaling)) {
        spdlog::error("services.get hook failed, Raitei cannot learn its server");
        return;
    }
    for (const auto& game : raitei::supported_games()) {
        const HMODULE game_module = GetModuleHandleW(game.module.data());
        if (game_module == nullptr) {
            continue;
        }
        spdlog::info("hooking {}", game.name);
        if (!game.install(game_module, runtime->tunnel, runtime->signaling)) {
            spdlog::error("{} hooks are incomplete, online matching will not work", game.name);
        }
        return;
    }
    spdlog::error("no supported game is loaded");
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        start(module);
    }
    return TRUE;
}
