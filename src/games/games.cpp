#include "game.h"

#include "ccj/ccj_hooks.h"
#include "iidx/iidx_hooks.h"
#include "sdvx/ess_hooks.h"

#include <array>

namespace raitei {

namespace {

constexpr std::array games{
    Game{"SOUND VOLTEX", L"ess.dll", &install_ess_hooks},
    Game{"beatmania IIDX", L"bm2dx.dll", &install_iidx_hooks},
    Game{"CHASE CHASE JOKERS", L"kamunity.dll", &install_ccj_hooks, L"game/chaseproject.exe"},
};

}

std::span<const Game> supported_games() {
    return games;
}

}
