#pragma once
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

/**
 * The once-per-session part of reading-stats accounting, separated from EpubReaderActivity so it
 * can be reasoned about and tested on its own.
 *
 * It exists because the reader has more than one way to end a session. The obvious one is
 * onExit(). The other is launching manual KOSync, which releases `epub` and `section` early to
 * free ~65 KB for the TLS handshake and only then replaces the activity -- so by the time
 * onExit() runs, the objects its stats commit needs are already gone and the whole session
 * (reading time, session count, and the buffered BookOrbit events) is silently dropped.
 *
 * The fix is to finalize before that early teardown as well, which immediately raises the
 * question this type answers: what stops the session being counted twice when onExit() then runs
 * normally? `finalized` is a one-way latch. beginFinalize() returns true exactly once per
 * session; every later call is a no-op, so both callers can be unconditional and neither has to
 * know whether the other already ran.
 *
 * Header-only and dependency-free (no Activity, GfxRenderer, Epub, or SD), so the double-count
 * rule is verified directly rather than inferred from the call sites.
 */
struct ReadingStatsSession {
  /**
   * A session shorter than this contributes reading time but is not counted as a session --
   * opening a book to check a word should not inflate the session count or the streak.
   */
  static constexpr uint32_t MIN_SESSION_SECONDS = 60;

  bool finalized = false;

  /** True exactly once per session. Subsequent calls are false, whoever makes them. */
  bool beginFinalize() {
    if (finalized) return false;
    finalized = true;
    return true;
  }

  /** Start of a new reading session; clears the latch. */
  void reset() { finalized = false; }

  /**
   * Fold one completed session's reading time into the per-book and global aggregates. Pure:
   * mutates only the two structs it is handed. A sub-threshold session still contributes its
   * seconds, matching the behaviour this replaced.
   */
  static void applySessionTotals(uint32_t sessionSeconds, BookReadingStats& book, GlobalReadingStats& global) {
    if (sessionSeconds < MIN_SESSION_SECONDS) return;
    book.sessionCount++;
    global.totalSessions++;
    book.totalReadingSeconds =
        book.totalReadingSeconds > UINT32_MAX - sessionSeconds ? UINT32_MAX : book.totalReadingSeconds + sessionSeconds;
    global.totalReadingSeconds = global.totalReadingSeconds > UINT32_MAX - sessionSeconds
                                     ? UINT32_MAX
                                     : global.totalReadingSeconds + sessionSeconds;
  }
};
