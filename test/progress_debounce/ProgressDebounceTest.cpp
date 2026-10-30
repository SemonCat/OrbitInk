#include <gtest/gtest.h>

#include "ProgressDebounce.h"

TEST(ProgressDebounceTest, FreshInstanceHasNoPendingChangeAndIsNotFlushDue) {
  ProgressDebounce debounce;
  EXPECT_FALSE(debounce.hasPendingChange());
  EXPECT_FALSE(debounce.isFlushDue(0));
  EXPECT_FALSE(debounce.isFlushDue(1000000));
}

TEST(ProgressDebounceTest, OneOrTwoChangedPagesDoNotTriggerFlushBeforeTimeElapses) {
  ProgressDebounce debounce;
  debounce.notePositionChanged(1000);
  EXPECT_TRUE(debounce.hasPendingChange());
  EXPECT_FALSE(debounce.isFlushDue(1000));

  debounce.notePositionChanged(1100);
  EXPECT_FALSE(debounce.isFlushDue(1200)) << "2 changed pages must not flush before the 3-page threshold";
}

TEST(ProgressDebounceTest, ThirdChangedPageTriggersFlush) {
  ProgressDebounce debounce;
  debounce.notePositionChanged(1000);
  debounce.notePositionChanged(1100);
  debounce.notePositionChanged(1200);
  EXPECT_TRUE(debounce.isFlushDue(1200)) << "3rd changed page must flush immediately, before any time threshold";
}

TEST(ProgressDebounceTest, SixtySecondsSinceFirstChangeTriggersFlushEvenWithOnlyOnePage) {
  ProgressDebounce debounce;
  const unsigned long firstChange = 5000;
  debounce.notePositionChanged(firstChange);

  EXPECT_FALSE(debounce.isFlushDue(firstChange + 59999)) << "must not flush 1ms before the 60s bound";
  EXPECT_TRUE(debounce.isFlushDue(firstChange + 60000)) << "must flush at exactly 60s since the first pending change";
}

TEST(ProgressDebounceTest, TimeWindowIsMeasuredFromTheFirstUnflushedChangeNotTheLatest) {
  ProgressDebounce debounce;
  debounce.notePositionChanged(0);
  // A second change 50s later must not reset the 60s clock -- otherwise a steady trickle of page
  // turns (fewer than 3 at a time) could withhold a flush indefinitely, violating the bounded-loss
  // guarantee ("a crash may lose at most the bounded unflushed interval").
  debounce.notePositionChanged(50000);
  EXPECT_FALSE(debounce.isFlushDue(59999));
  EXPECT_TRUE(debounce.isFlushDue(60000));
}

TEST(ProgressDebounceTest, MarkFlushedResetsBothCountersAndTheTimeWindow) {
  ProgressDebounce debounce;
  debounce.notePositionChanged(1000);
  debounce.notePositionChanged(1100);
  debounce.markFlushed();

  EXPECT_FALSE(debounce.hasPendingChange());
  EXPECT_FALSE(debounce.isFlushDue(1000000)) << "a stale firstChangeMs from before markFlushed() must not leak through";

  // A fresh change after flushing restarts the window from ITS OWN timestamp, not the old one.
  debounce.notePositionChanged(2000);
  EXPECT_FALSE(debounce.isFlushDue(2000 + 59999));
  EXPECT_TRUE(debounce.isFlushDue(2000 + 60000));
}

TEST(ProgressDebounceTest, ChangedPageCounterSaturatesInsteadOfOverflowing) {
  ProgressDebounce debounce;
  // Far more than the flush threshold; must not wrap the internal uint8_t counter back to 0 and
  // spuriously report "no pending change".
  for (int i = 0; i < 1000; i++) {
    debounce.notePositionChanged(0);
  }
  EXPECT_TRUE(debounce.hasPendingChange());
  EXPECT_TRUE(debounce.isFlushDue(0));
}
