#pragma once

// Owned simulator adapter: additive extension of crosspoint-simulator's HalSystem.
//
// HalSystem is a namespace, not a class, so unlike HalClock/HalStorage this needs no shadow copy
// at all -- #include_next pulls in the simulator's declarations unchanged (this adapter library is
// searched before the simulator's src/, so this file is found first for `#include <HalSystem.h>`
// and #include_next continues past it), and the one CrossFade-only entry point is simply declared
// alongside them. Nothing here can drift out of sync with the pinned dependency.
#include_next <HalSystem.h>

namespace HalSystem {

// Mirrors lib/hal/HalSystem.h. Called as the very first statement in setup() (src/main.cpp:305).
// Defined in HalSystemCompat.cpp.
void recordBootIdentity();

}  // namespace HalSystem
