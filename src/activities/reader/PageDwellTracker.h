#pragma once
#include <cstdint>

/**
 * Measures how long the page currently on screen has been displayed, and decides whether that
 * counts as reading.
 *
 * Split out of EpubReaderActivity for one reason: the rule it enforces -- that EVERY way of
 * leaving a page closes that page's dwell, and that a page's time is credited to the page that
 * earned it -- is easy to state, easy to break, and impossible to test through the Activity,
 * which needs a renderer, an SD card and an EPUB before it will run at all. Here it is a plain
 * struct with an injected clock, so the navigation paths that once carried a page's dwell across
 * a jump (chapter skip, percentage jump, bookmarks, links, restores, reflow) can be regression
 * tested directly.
 *
 * Wraparound: millis() wraps every ~49.7 days. The elapsed computation is a plain unsigned
 * subtraction, which is exact across a single wrap by modular arithmetic.
 *
 * Timestamps are uint32_t rather than the `unsigned long` millis() returns. On the ESP32 those
 * are the same 32-bit type, but on a 64-bit host `unsigned long` is 64 bits wide and the
 * subtraction would not wrap at all -- so the wraparound behaviour verified by the host tests
 * would not be the behaviour that ships. Fixing the width makes both identical.
 */
struct PageDwellTracker {
  /**
   * Dwell longer than this means the reader was left open rather than actively read (the device
   * sleeps well before it, so in practice this catches a paused session), and shorter than
   * MIN_DWELL_SECONDS means an accidental or instant double-turn. Both are discarded.
   */
  static constexpr uint32_t IDLE_THRESHOLD_SECONDS = 5 * 60;
  static constexpr uint32_t MIN_DWELL_SECONDS = 2;

  /** Start timing the page now. Also the reset used when tracking is off. */
  void pageShown(uint32_t nowMs) {
    shownAtMs = nowMs;
    started = true;
  }

  /**
   * Close out the page being left and start timing its replacement, returning the QUALIFYING
   * dwell in seconds -- 0 when it does not qualify, or when no page was being timed.
   *
   * Always re-baselines, which is what makes a redundant call harmless: leaving a page twice in
   * a row measures zero the second time, so the navigation hooks can be unconditional without
   * any of them needing to know whether another already fired.
   */
  uint32_t leavePage(uint32_t nowMs) {
    const bool wasTiming = started;
    const uint32_t elapsedMs = nowMs - shownAtMs;  // unsigned 32-bit: correct across a wrap
    pageShown(nowMs);
    if (!wasTiming) return 0;

    const uint32_t elapsedSeconds = elapsedMs / 1000u;
    if (elapsedSeconds < MIN_DWELL_SECONDS || elapsedSeconds > IDLE_THRESHOLD_SECONDS) return 0;
    return elapsedSeconds;
  }

  uint32_t shownAtMs = 0;
  bool started = false;
};
