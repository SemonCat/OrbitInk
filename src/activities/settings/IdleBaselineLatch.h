#pragma once

// One-shot gate for "a stretch of main-loop-blocking work just ended, so re-base the auto-sleep
// inactivity timer" (see ActivityManager::requestIdleTimerReset).
//
// Pure state, no dependencies, so the transition rules can be tested without the activity stack.
//
// Why it needs its own arming signal rather than just "is the state terminal?": the work is
// synchronous. A font download runs to completion inside a single loop() call, so the enclosing
// loop only ever observes the state BEFORE and AFTER -- never DOWNLOADING in between. Inferring
// "work happened" from having previously observed a non-terminal state gets this wrong on a
// retry started from the ERROR screen (ERROR -> DOWNLOADING -> terminal, all within one call):
// the observer sees terminal both times, concludes nothing happened, and skips the re-base --
// after which a long retry is followed by an immediate auto-sleep, because the inactivity
// baseline is stale by the whole download. Arming explicitly where the work starts removes the
// inference entirely.
namespace idlebaseline {

class Latch {
 public:
  // Call where blocking work begins (entering a download or a manifest fetch), before it runs.
  // Idempotent: re-arming while already armed is a no-op.
  void noteWorkStarted() { armed_ = true; }

  // Call once per loop pass with whether the activity is now showing a terminal result screen.
  // Returns true exactly once per stretch of work -- on the first terminal observation after it.
  bool observeTerminal(const bool terminal) {
    if (!terminal || !armed_) return false;
    armed_ = false;
    return true;
  }

  // Whether a re-base is still owed. Exposed for tests and diagnostics.
  bool armed() const { return armed_; }

 private:
  bool armed_ = false;
};

}  // namespace idlebaseline
