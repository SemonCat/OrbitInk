#pragma once

// Shared by JpegToFramebufferConverter and PngToFramebufferConverter's draw callbacks: a large or
// slow-to-decode image can hold the render task (and with it the caller's RenderLock, still
// legitimately held -- the framebuffer is mid-write) for long enough to trip the watchdog.
// vTaskDelay yields the CPU without releasing anything; it neither unlocks RenderLock nor touches
// renderGeneration, so stale-render detection (RenderLock::isStale()) still only takes effect at
// the next lock.isStale() check after decode() returns, same as before this yield existed.
//
// `lastYieldMs` is a per-decode state variable (reset to 0 at the start of each decode, e.g. via
// a context struct's default member initializer) owned by the caller -- this call is cheap enough
// to make unconditionally from inside a tight per-block/per-row callback.
inline void yieldPeriodicallyDuringDecode(unsigned long& lastYieldMs, unsigned long intervalMs = 250) {
  const unsigned long nowMs = millis();
  if (nowMs - lastYieldMs >= intervalMs) {
    lastYieldMs = nowMs;
    vTaskDelay(1);
  }
}
