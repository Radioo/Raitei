#pragma once

#include "core/signaling.h"
#include "core/tunnel.h"

#include <windows.h>

namespace raitei {

bool install_iidx_hooks(HMODULE game, Tunnel& tunnel, SignalingClient& signaling);

}
