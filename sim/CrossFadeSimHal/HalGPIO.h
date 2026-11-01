#pragma once

// Owned simulator adapter: superset shadow of crosspoint-simulator's HalGPIO.
//
// WHY THIS FILE EXISTS
// The simulator replaces the firmware HAL wholesale (`lib_ignore = hal`), so on the host
// `#include <HalGPIO.h>` resolves here instead of lib/hal/HalGPIO.h. CrossFade's HalGPIO carries a
// `WakeupReason::Timer` enumerator the pinned simulator does not (src/main.cpp:346 compares
// against it directly, unconditionally, on every boot path). An enumerator cannot be added to an
// enum from outside it, and the simulator checkout under .pio/libdeps must not be patched -- so
// this header declares the class as the pinned simulator declares it, plus that enumerator.
//
// MAINTENANCE CONTRACT
// This mirrors crosspoint-simulator@dacbbbc src/HalGPIO.h declaration for declaration -- same
// members, same order, same inline bodies -- but reformatted to this repo's clang-format style, so
// compare declarations rather than bytes. The simulator's own HalGPIO.cpp includes its copy via a
// quoted include and so keeps compiling against the original; this shadow is what the firmware
// sees. The two definitions must agree on data layout, because the object itself (`gpio`) is
// constructed in the simulator's translation unit. The exact dependency pin in platformio.ini is
// what keeps that true. When bumping the pin, re-diff this file against the new src/HalGPIO.h
// before anything else.
//
// `WakeupReason::Timer` is the one addition, and it does not touch layout: HalGPIO has no member
// of type WakeupReason (the enum is purely a return-value type for getWakeupReason()), so adding
// an enumerator changes neither sizeof(HalGPIO) nor any inline body. That is also why this
// enumerator is inserted at the point it mirrors upstream's nesting, rather than "at the end of the
// class" the way sim/README.md prescribes for real added members -- there is no member here to
// append.
//
// The simulator's getWakeupReason() (defined in its own .cpp, unmodified) never returns Timer. The
// owned crossfadeWakeupReason() below does, for a process that HalPowerManagerCompat.cpp relaunched
// from an accelerated timer sleep (opt-in, see sim/README.md); otherwise it returns exactly what the
// simulator's own function returns. The firmware reaches it through the getWakeupReason macro at
// the end of this file -- a member's definition cannot be replaced from outside the dependency, and
// the simulator's own translation units never include this header, so they are unaffected.
//
// See sim/README.md.

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#ifndef EPD_SCLK
#define EPD_SCLK 8  // SPI Clock
#endif
#ifndef EPD_MOSI
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#endif
#ifndef EPD_CS
#define EPD_CS 21  // Chip Select
#endif
#ifndef EPD_DC
#define EPD_DC 4  // Data/Command
#endif
#ifndef EPD_RST
#define EPD_RST 5  // Reset
#endif
#ifndef EPD_BUSY
#define EPD_BUSY 6  // Busy
#endif

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Clears the per-frame press/release edge latches. Must be called exactly
  // once per frame (before the firmware's loop()), NOT on every update().
  void beginFrame();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  bool isDebouncePending() const { return false; }
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  bool hasHomeKey() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float &nx, float &ny) const;
  bool wasTouchDown(float &nx, float &ny) const;
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float &nx, float &ny, unsigned long &heldMs) const;
  bool isTouchHeldAt(float &nx, float &ny) const;
  bool wasTouchLongPress(float &nx, float &ny) const;
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float &nxStart, float &nyStart, float &nxEnd, float &nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);
  bool restoreTouchAfterDisplayReset();
  bool consumeSimulatorSleepRequest();

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // The host wake path is synthetic, so verification always succeeds.
  bool verifyPowerButtonWakeup();
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;
  void pollUsbState() {}

  // ---- CROSSFADE ADDITIONS ----
  // `Timer` is not present upstream. The simulator's getWakeupReason() (unmodified, defined in its
  // own .cpp) never produces it -- see the file header comment above.
  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other, Timer };

  WakeupReason getWakeupReason() const;
  // Defined in HalGPIOCompat.cpp. Non-virtual and not a data member, so the layout is unchanged.
  WakeupReason crossfadeWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;  // Singleton

// Layout guard. This pins the size of *this* declaration, which is the one thing a future edit is
// most likely to get wrong: with CROSSPOINT_EMULATED=1 (the only mode the simulator build uses),
// the sole data member is `_deviceType` (a uint8_t-backed enum), so sizeof(HalGPIO) == 1. It cannot
// see the simulator's own header, so it does NOT detect upstream drift -- bumping the pin still
// requires the manual re-diff described above.
static_assert(sizeof(HalGPIO) == 1,
              "HalGPIO layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalGPIO.h -- never against lib/hal/HalGPIO.h.");

// Firmware calls `gpio.getWakeupReason()` (src/main.cpp); route them to the timer-aware member.
#define getWakeupReason crossfadeWakeupReason
