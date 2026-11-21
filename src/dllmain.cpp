#include "ess_hooks.h"
#include "services_hook.h"
#include "signaling.h"
#include "tunnel.h"

#include <windows.h>

#include <rtc/rtc.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <memory>

namespace {

constexpr std::wstring_view log_file_name = L"raitei.log";

struct Runtime {
    Runtime() : tunnel(signaling) {
    }

    raitei::SignalingClient signaling;
    raitei::Tunnel tunnel;
};

std::filesystem::path module_directory(HMODULE module) {
    std::array<wchar_t, MAX_PATH> path{};
    GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(path.data()).parent_path();
}

void start(HMODULE module) {
    auto logger = spdlog::basic_logger_mt("raitei", (module_directory(module) / log_file_name).string(), true);
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
    if (!raitei::install_ess_hooks(runtime->tunnel, runtime->signaling)) {
        spdlog::error("ess hooks are incomplete, online matching will not work");
    }
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        start(module);
    }
    return TRUE;
}
