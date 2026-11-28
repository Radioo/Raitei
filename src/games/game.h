#pragma once

#include "core/signaling.h"
#include "core/tunnel.h"

#include <windows.h>

#include <span>
#include <string_view>

namespace raitei {

struct Game {
    std::string_view name;
    std::wstring_view module;
    bool (*install)(HMODULE module, Tunnel& tunnel, SignalingClient& signaling);
    std::wstring_view required_file{};
};

std::span<const Game> supported_games();

}
