#include <gtest/gtest.h>

#include "IdleBaselineLatch.h"

using idlebaseline::Latch;

namespace {
// FontDownloadActivity's states, reduced to what the latch cares about.
constexpr bool kTerminal = true;      // COMPLETE / ERROR
constexpr bool kNotTerminal = false;  // WIFI_SELECTION / LOADING_MANIFEST / FAMILY_LIST / DOWNLOADING
}  // namespace

// THE regression. Retry is offered on the ERROR screen, and it calls downloadFamily() directly:
// ERROR -> DOWNLOADING -> COMPLETE all happen inside one loop() call, so the enclosing loop
// observes a terminal state before AND after and never sees the work. A gate that infers "work
// happened" from having observed a non-terminal state stays latched from the first ERROR and
// skips the re-base -- so a long retry ends with an inactivity baseline stale by the whole
// download, and the device can sleep immediately over its own result screen.
TEST(IdleBaselineLatch, RetryStartedFromTheErrorScreenStillFires) {
  Latch latch;

  // First download fails.
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal));
  // Sitting on the ERROR screen must not fire again.
  EXPECT_FALSE(latch.observeTerminal(kTerminal));

  // Retry: no non-terminal state is ever observed from outside, only the arming call inside
  // downloadFamily().
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal)) << "retry's own long download got no re-base";
}

TEST(IdleBaselineLatch, FiresExactlyOncePerStretchOfWork) {
  Latch latch;
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal));
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(latch.observeTerminal(kTerminal)) << "re-based again on loop pass " << i;
  }
}

// Nothing blocking has run, so there is no stale baseline to correct: a terminal state on its
// own must not reset the inactivity timer (that would keep the device awake for free).
TEST(IdleBaselineLatch, DoesNotFireWithoutWork) {
  Latch latch;
  EXPECT_FALSE(latch.armed());
  EXPECT_FALSE(latch.observeTerminal(kTerminal));
}

// The normal path: browsing the family list, then a download that ends in a result screen.
TEST(IdleBaselineLatch, NormalDownloadFromTheListFires) {
  Latch latch;
  EXPECT_FALSE(latch.observeTerminal(kNotTerminal));  // FAMILY_LIST passes
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal));
}

// Work that lands somewhere non-terminal (a manifest fetch that succeeds into FAMILY_LIST)
// leaves the re-base owed; it is paid at the next terminal screen, not dropped.
TEST(IdleBaselineLatch, WorkEndingNonTerminalStaysArmed) {
  Latch latch;
  latch.noteWorkStarted();
  EXPECT_FALSE(latch.observeTerminal(kNotTerminal));
  EXPECT_TRUE(latch.armed());
  EXPECT_TRUE(latch.observeTerminal(kTerminal));
}

// Arming is idempotent -- downloadAll() drives downloadFamily() once per family, and the manifest
// path can arm before a download arms again.
TEST(IdleBaselineLatch, RepeatedArmingStillFiresOnlyOnce) {
  Latch latch;
  latch.noteWorkStarted();
  latch.noteWorkStarted();
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal));
  EXPECT_FALSE(latch.observeTerminal(kTerminal));
}

// The manifest-fetch failure path calls the observer twice for the same terminal state: once
// directly from the result handler (which runs after loop(), too late to wait a pass) and once
// from the loop() wrapper. It must re-base once, not twice.
TEST(IdleBaselineLatch, DoubleObservationOfTheSameTerminalStateFiresOnce) {
  Latch latch;
  latch.noteWorkStarted();
  EXPECT_TRUE(latch.observeTerminal(kTerminal));   // result handler
  EXPECT_FALSE(latch.observeTerminal(kTerminal));  // loop() wrapper, same pass
}
