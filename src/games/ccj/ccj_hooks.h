#pragma once

#include "core/signaling.h"
#include "core/tunnel.h"

#include <windows.h>

namespace raitei {

bool install_ccj_hooks(HMODULE kamunity, Tunnel& tunnel, SignalingClient& signaling);

}
