// Owned simulator adapter: the HalSystem entry point CrossFade adds on top of the pinned
// crosspoint-simulator HAL. See sim/README.md for the boundary rules.

#include <HalSystem.h>

namespace HalSystem {

// Device behaviour: stamps this build's identity (app ELF SHA-256) into RTC_NOINIT memory and
// records whether it matches the identity left by whatever last ran setup(), which is how
// isRebootFromPanic() tells a crash-reboot of the same image from a fresh flash of a new one.
//
// A host process has neither an app partition to hash nor RTC memory that survives it: every run
// is a cold start of a freshly built image. The simulator's own isRebootFromPanic() already
// reports false unconditionally, which is the consistent answer, so recording an identity would
// have nothing to compare against and nothing to influence. Deliberately a no-op.
//
// What this costs in simulator fidelity: the panic-recovery boot path (src/main.cpp:485 and 545,
// CrashActivity) cannot be reached from the simulator. Test it on device or through the host gtest
// suites, not here.
void recordBootIdentity() {}

}  // namespace HalSystem
