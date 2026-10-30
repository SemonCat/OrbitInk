// Fake-clock tests for the REAL src/activities/reader/PageDwellTracker.h.
//
// The property under test is the one that non-page-turn navigation kept breaking: a page's dwell
// belongs to the page that was on screen, so every way of leaving a page -- chapter skip
// (including the long-press variant), percentage jump, bookmark, followed link, position
// restore, orientation reflow -- must close the outgoing dwell before the replacement appears.
// Miss one and the outgoing page's time is credited to whatever lands next, and its BookOrbit
// event is stamped at the wrong position.
//
// Time is injected, so these are exact rather than timing-dependent.

#include <gtest/gtest.h>

#include "PageDwellTracker.h"

namespace {

constexpr uint32_t SEC = 1000u;

TEST(PageDwellTrackerTest, MeasuresTheTimeAPageWasOnScreen) {
  PageDwellTracker dwell;
  dwell.pageShown(10 * SEC);
  EXPECT_EQ(dwell.leavePage(40 * SEC), 30u);
}

TEST(PageDwellTrackerTest, TimingHasNotStartedUntilAPageIsShown) {
  PageDwellTracker dwell;
  EXPECT_EQ(dwell.leavePage(60 * SEC), 0u) << "no page was being timed, so there is nothing to credit";
}

TEST(PageDwellTrackerTest, ADwellBelowTheMinimumIsDiscarded) {
  // An accidental or instant double-turn is not reading.
  PageDwellTracker dwell;
  dwell.pageShown(0);
  EXPECT_EQ(dwell.leavePage((PageDwellTracker::MIN_DWELL_SECONDS - 1) * SEC), 0u);

  dwell.pageShown(0);
  EXPECT_EQ(dwell.leavePage(PageDwellTracker::MIN_DWELL_SECONDS * SEC), PageDwellTracker::MIN_DWELL_SECONDS)
      << "the threshold itself counts";
}

TEST(PageDwellTrackerTest, ADwellPastTheIdleThresholdIsDiscarded) {
  // The reader was left open rather than actively read.
  PageDwellTracker dwell;
  dwell.pageShown(0);
  EXPECT_EQ(dwell.leavePage((PageDwellTracker::IDLE_THRESHOLD_SECONDS + 1) * SEC), 0u);

  dwell.pageShown(0);
  EXPECT_EQ(dwell.leavePage(PageDwellTracker::IDLE_THRESHOLD_SECONDS * SEC), PageDwellTracker::IDLE_THRESHOLD_SECONDS)
      << "the threshold itself still counts";
}

// ---- The navigation invariant ------------------------------------------------------------------

TEST(PageDwellTrackerTest, LeavingAPageDoesNotCarryItsTimeOntoTheNextOne) {
  // The exact bug the page-leave hook exists to prevent: five minutes on one page, then a jump,
  // then a few seconds on the page it landed on. Without the hook the whole five minutes would
  // be credited to the destination -- and, for BookOrbit, stamped at the destination's position.
  PageDwellTracker dwell;
  dwell.pageShown(0);

  const uint32_t outgoing = dwell.leavePage(240 * SEC);  // the jump closes the page being left
  EXPECT_EQ(outgoing, 240u);

  const uint32_t incoming = dwell.leavePage(245 * SEC);  // then the destination is read briefly
  EXPECT_EQ(incoming, 5u) << "the destination must be credited only with its own five seconds";
}

TEST(PageDwellTrackerTest, ARedundantPageLeaveMeasuresZeroSoHooksCanBeUnconditional) {
  // Several navigation paths funnel through more than one hook (a long-press chapter skip that
  // also reflows, say). Each call re-baselines, so the second one contributes nothing rather
  // than double-counting -- which is what lets every path call the hook without coordinating.
  PageDwellTracker dwell;
  dwell.pageShown(0);

  EXPECT_EQ(dwell.leavePage(100 * SEC), 100u);
  EXPECT_EQ(dwell.leavePage(100 * SEC), 0u) << "an immediate second leave has measured no reading";
  EXPECT_EQ(dwell.leavePage(100 * SEC), 0u);
}

TEST(PageDwellTrackerTest, ASequenceOfJumpsCreditsEachPageSeparately) {
  // Models a real session: read, chapter skip, read, bookmark jump, read.
  PageDwellTracker dwell;
  dwell.pageShown(0);

  EXPECT_EQ(dwell.leavePage(30 * SEC), 30u);   // chapter skip
  EXPECT_EQ(dwell.leavePage(50 * SEC), 20u);   // bookmark jump
  EXPECT_EQ(dwell.leavePage(110 * SEC), 60u);  // page turn

  // Total credited is the wall time actually spent, with nothing double counted.
  EXPECT_EQ(30u + 20u + 60u, 110u);
}

TEST(PageDwellTrackerTest, AnIdlePageBetweenTwoRealOnesDoesNotPoisonEither) {
  PageDwellTracker dwell;
  dwell.pageShown(0);
  EXPECT_EQ(dwell.leavePage(20 * SEC), 20u);
  // Left open over lunch: discarded, but it must still re-baseline.
  EXPECT_EQ(dwell.leavePage(20 * SEC + (PageDwellTracker::IDLE_THRESHOLD_SECONDS + 60) * SEC), 0u);
  EXPECT_EQ(dwell.leavePage(20 * SEC + (PageDwellTracker::IDLE_THRESHOLD_SECONDS + 90) * SEC), 30u)
      << "the page after an idle stretch is timed normally";
}

TEST(PageDwellTrackerTest, MeasurementIsCorrectAcrossAMillisWraparound) {
  // millis() wraps every ~49.7 days; a signed subtraction here would produce a ~49-day dwell,
  // which the idle threshold would silently discard -- losing the page rather than crediting it.
  PageDwellTracker dwell;
  dwell.pageShown(0xFFFFFF00u);
  // 0x100 ms to the wrap, then 9744 ms past it: 10000 ms total.
  EXPECT_EQ(dwell.leavePage(9744u), 10u);
}

}  // namespace
