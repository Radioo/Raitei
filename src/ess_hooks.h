#pragma once

#include "signaling.h"
#include "tunnel.h"

namespace raitei {

bool install_ess_hooks(Tunnel& tunnel, const SignalingClient& signaling);

}
