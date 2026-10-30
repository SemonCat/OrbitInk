#pragma once

// Owned simulator adapter: re-export the production header.
//
// `lib_ignore = hal` in [env:simulator_x3] drops all of lib/hal/ so the simulator can supply its
// own HalDisplay/HalStorage/HalGPIO/... That also removes CalendarTime.h, which is not a HAL at
// all -- it is pure constexpr civil-calendar arithmetic with no hardware or platform dependency
// (see lib/hal/CalendarTime.h), and lib/KOReaderSync/BookOrbitBookmarks.h needs it on the host
// exactly as it does on the device.
//
// Forwarding rather than copying is deliberate: there is one definition, the simulator build
// compiles the same code the device does, and this file can never drift from it.
#include "../../lib/hal/CalendarTime.h"
