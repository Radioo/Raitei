#pragma once

#include "core/signaling.h"
#include "core/tunnel.h"

#include <windows.h>

namespace raitei {

bool install_ess_hooks(HMODULE ess, Tunnel& tunnel, SignalingClient& signaling);

}
