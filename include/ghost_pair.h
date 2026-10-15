#pragma once
#include <Arduino.h>
#if GHOST_NODE_ROLE == 2 && __has_include("ghost_relay_private.h")
#include "ghost_relay_private.h"
#elif GHOST_NODE_ROLE != 2 && __has_include("ghost_pair_private.h")
#include "ghost_pair_private.h"
#else

// ---------------------------------------------------------------------------
// GHOSTLINK PAIR CONFIG
// ---------------------------------------------------------------------------
// This file is intentionally NOT a usable public default.
//
// 1. Flash either env once with GHOST_PAIR_CONFIGURED = 0.
// 2. Read each board's Wi-Fi station MAC from the splash screen / serial.
// 3. Run:
//      python3 tools/make_pair.py --a AA:BB:CC:DD:EE:01 --b AA:BB:CC:DD:EE:02
// 4. Save output to include/ghost_pair_private.h (ignored by Git).
// 5. Flash env:ghost-a to board A, env:ghost-b to board B.
// ---------------------------------------------------------------------------

#define GHOST_PAIR_CONFIGURED 0

namespace ghostpair {

constexpr uint16_t NETWORK_ID = 0x0000;
constexpr uint8_t NODE_A_MAC[6] = {0, 0, 0, 0, 0, 0};
constexpr uint8_t NODE_B_MAC[6] = {0, 0, 0, 0, 0, 0};

// PMK is retained for provisioning-file compatibility. LMK is the endpoint
// chat secret; relay exports must not contain it.
constexpr uint8_t PMK[16] = {0};
constexpr uint8_t LMK[16] = {0};

// Network membership key for hopping and transport/BLE authentication.
constexpr uint8_t HOP_KEY[32] = {0};

} // namespace ghostpair
#endif
