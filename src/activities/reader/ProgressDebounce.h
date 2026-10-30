#pragma once

#include <cstdint>

// Decides WHEN accumulated reading-progress changes should be written to disk, instead of on
// every page turn (SPIFFS/SD write throttling, see CLAUDE.md). Pure decision logic -- no I/O, no
// Arduino types -- so it is host-testable in isolation; callers own the actual write and only
// call markFlushed() after a CONFIRMED successful one.
//
// Bounded-loss policy: flush after 3 changed pages or 60s since the first unflushed change,
// whichever comes first. A crash may lose at most that bounded interval. Callers MUST still force
// an unconditional flush (bypassing this policy) before reader exit, deep sleep, book
// transitions, cache-invalidating operations, and any KOSync request -- this class only decides
// the OPPORTUNISTIC flush cadence during ordinary reading, not those hard flush points.
class ProgressDebounce {
 public:
  static constexpr uint8_t kFlushAfterChangedPages = 3;
  static constexpr unsigned long kFlushAfterMs = 60000;

  // Call whenever the in-memory reading position changes to a new distinct page/spine.
  void notePositionChanged(unsigned long nowMs) {
    if (changedPages == 0) firstChangeMs = nowMs;
    if (changedPages < UINT8_MAX) changedPages++;
  }

  // Poll independently of notePositionChanged (e.g. once per loop tick) so a lone pending change
  // still flushes within kFlushAfterMs even if the reader never turns another page.
  bool isFlushDue(unsigned long nowMs) const {
    return changedPages > 0 && (changedPages >= kFlushAfterChangedPages || (nowMs - firstChangeMs) >= kFlushAfterMs);
  }

  bool hasPendingChange() const { return changedPages > 0; }

  // Call only after a confirmed successful write.
  void markFlushed() { changedPages = 0; }

 private:
  uint8_t changedPages = 0;
  unsigned long firstChangeMs = 0;
};
