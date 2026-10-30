#pragma once

// Owned simulator adapter: mirror of crosspoint-simulator's HalTiltSensor, retargeting its
// internal HalGPIO.h include.
//
// WHY THIS FILE EXISTS
// This header carries no CrossFade additions of its own -- it exists solely to break a collision
// introduced by sim/CrossFadeSimHal/HalGPIO.h (see that file for why HalGPIO needs a superset
// shadow). The pinned crosspoint-simulator's own HalTiltSensor.h quote-includes "HalGPIO.h", which
// always resolves relative to *its own* directory (.pio/libdeps/.../simulator/src/), regardless of
// this library's -I precedence -- that is how quoted includes work, not a PlatformIO quirk. Every
// translation unit that includes both <HalTiltSensor.h> and <HalGPIO.h> (src/main.cpp,
// src/CrossPointSettings.cpp via SettingsList.h, src/activities/reader/ReaderUtils.h, and others)
// would therefore end up with two distinct definitions of `class HalGPIO` in the same translation
// unit -- the shadow (via the angled include) and the pinned original (via HalTiltSensor.h's
// quoted include) -- a hard "redefinition of HalGPIO" error, since a class cannot be reopened.
//
// The fix is this header: because CrossFadeSimHal's directory precedes the simulator's in the
// include search path, `<HalTiltSensor.h>` resolves here instead, and *this* copy quote-includes
// "HalGPIO.h" relative to sim/CrossFadeSimHal/ -- i.e. the HalGPIO shadow -- so every consumer in
// the program converges on the exact same HalGPIO definition. No behavior changes: HalTiltSensor's
// members and every inline body are reproduced verbatim (reformatted to this repo's clang-format
// style) from crosspoint-simulator@dacbbbc src/HalTiltSensor.h.
//
// The simulator's own HalTiltSensor.cpp includes its copy through a quoted include, which always
// resolves to its own directory first, so it keeps compiling -- and constructing `halTiltSensor` --
// against the original. The two class bodies must therefore keep matching member-for-member and
// inline-body-for-inline-body, the same maintenance contract as every other shadow in this
// directory. When bumping the simulator pin, re-diff this file against the new
// src/HalTiltSensor.h before anything else.
//
// See sim/README.md and sim/CrossFadeSimHal/HalGPIO.h.

#include <Arduino.h>

#include "HalGPIO.h"

namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

class HalTiltSensor {
 private:
  bool _available = false;
  bool _isAwake = false;

 public:
  void begin() {
#if defined(SIMULATOR_DEVICE_X3) || defined(SIMULATOR_DEVICE_X4_CLASSIC) || \
    defined(SIMULATOR_DEVICE_STICKY)
    _available = true;
#else
    _available = false;
#endif
    _isAwake = false;
  }

  bool wake() {
    if (!_available) return false;
    _isAwake = true;
    return true;
  }

  bool deepSleep() {
    if (!_available) return false;
    _isAwake = false;
    return true;
  }

  bool isAvailable() const { return _available; }
  // Support both firmware HAL shapes while the repos are out of sync.
  void update(const uint8_t /*mode*/, const uint8_t /*orientation*/, const bool /*inReader*/) {}
  void update(const uint8_t mode, const uint8_t /*direction*/, const uint8_t orientation,
              const bool inReader) {
    update(mode, orientation, inReader);
  }
  bool wasTiltedForward() { return false; }
  bool wasTiltedBack() { return false; }
  bool hadActivity() { return false; }
  void clearPendingEvents() {}
};

// Layout guard, same rationale as the other shadows in this directory: two bools, no padding
// between them, no virtual table.
static_assert(sizeof(HalTiltSensor) == 2,
              "HalTiltSensor layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalTiltSensor.h.");
