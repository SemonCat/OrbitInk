// Tests for the REAL lib/KOReaderSync/BookOrbitSessionClock.h.
//
// The X3's HalClock exposes whole epoch MINUTES only, while BookOrbit needs epoch SECONDS. This
// clock bridges that without patching the HAL and without an extra NTP round trip, by anchoring
// on one RTC reading and projecting with millis(). The two things that can go badly wrong --
// a millis() wrap mid-session, and a DS3231 that lost power and came back at its epoch default
// -- are the substance of this file.

#include <gtest/gtest.h>

#include "BookOrbitSessionClock.h"

namespace {

// 2026-09-03 00:00 UTC, comfortably inside the plausible window.
constexpr uint32_t VALID_MINUTES = CalendarTime::epochMinutes(2026, 9, 3, 0, 0);

TEST(BookOrbitSessionClockTest, UnanchoredClockYieldsNothing) {
  const BookOrbitSessionClock clock;
  uint32_t seconds = 0;
  EXPECT_FALSE(clock.isAnchored());
  EXPECT_FALSE(clock.now(1000, seconds));
}

TEST(BookOrbitSessionClockTest, ProjectsSecondsForwardFromTheAnchor) {
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 500000u));

  uint32_t seconds = 0;
  ASSERT_TRUE(clock.now(500000u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u) << "at the anchor instant, projection is the RTC reading";

  ASSERT_TRUE(clock.now(500000u + 90'000u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u + 90u) << "90 s of millis must project to 90 s of wall clock";
}

TEST(BookOrbitSessionClockTest, SubSecondMillisDoesNotAdvanceTheSecondsValue) {
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 1000u));
  uint32_t seconds = 0;
  ASSERT_TRUE(clock.now(1999u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u) << "truncation, never rounding up into the next second";
}

TEST(BookOrbitSessionClockTest, ProjectionIsCorrectAcrossAMillisWraparound) {
  // millis() wraps every ~49.7 days. Anchoring just before the wrap and reading just after must
  // still produce the right elapsed time -- a signed subtraction here would produce a ~49-day
  // jump backwards and poison every event in the session.
  constexpr uint32_t justBeforeWrap = 0xFFFFFF00u;
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, justBeforeWrap));

  uint32_t seconds = 0;
  // 0x100 ms to reach the wrap, then 2000 ms past it: 2256 ms total -> 2 whole seconds.
  ASSERT_TRUE(clock.now(2000u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u + 2u);
}

TEST(BookOrbitSessionClockTest, RejectsAnRtcThatLostPowerAndCameBackAtItsDefault) {
  // A DS3231 that lost power reports a syntactically valid time. It would satisfy the server's
  // own startTime >= 1 check and silently fill the user's stats with year-2000 sessions, so the
  // plausibility floor is the only thing standing between that and corrupted history.
  BookOrbitSessionClock clock;
  EXPECT_FALSE(clock.anchor(CalendarTime::epochMinutes(2000, 1, 1, 0, 0), 1000u));
  EXPECT_FALSE(clock.isAnchored());

  EXPECT_FALSE(clock.anchor(0, 1000u)) << "a zeroed RTC is not a time";
  EXPECT_FALSE(clock.anchor(CalendarTime::epochMinutes(2023, 12, 31, 23, 59), 1000u));
  EXPECT_TRUE(clock.anchor(CalendarTime::epochMinutes(2024, 1, 1, 0, 0), 1000u)) << "the floor itself is valid";
}

TEST(BookOrbitSessionClockTest, RejectsAbsurdFutureReadings) {
  BookOrbitSessionClock clock;
  EXPECT_FALSE(clock.anchor(CalendarTime::epochMinutes(2150, 1, 1, 0, 0), 1000u));
  EXPECT_FALSE(clock.isAnchored());
}

TEST(BookOrbitSessionClockTest, PlausibleWindowKeepsTheAnchorInsideUint32Seconds) {
  // anchorEpochMinutes * 60 must not overflow: uint32_t seconds saturate in 2106.
  EXPECT_LT(static_cast<uint64_t>(BookOrbitSessionClock::MAX_VALID_EPOCH_MINUTES) * 60u,
            static_cast<uint64_t>(UINT32_MAX));
}

TEST(BookOrbitSessionClockTest, ReconcileIgnoresOrdinaryMinuteQuantisation) {
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 0u));

  // 90 s later the RTC has ticked one minute; the projection is ahead of it by 30 s, which is
  // just the minutes-only granularity and must NOT cause a re-anchor.
  clock.reconcile(VALID_MINUTES + 1, 90'000u);
  uint32_t seconds = 0;
  ASSERT_TRUE(clock.now(90'000u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u + 90u);
}

TEST(BookOrbitSessionClockTest, ReconcileReAnchorsAfterALargeDivergence) {
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 0u));

  // The device slept: millis() barely moved but the RTC advanced an hour.
  clock.reconcile(VALID_MINUTES + 60, 5'000u);
  uint32_t seconds = 0;
  ASSERT_TRUE(clock.now(5'000u, seconds));
  EXPECT_EQ(seconds, (VALID_MINUTES + 60) * 60u) << "the fresh RTC reading must win";
}

TEST(BookOrbitSessionClockTest, ReconcileWithAnImplausibleReadingChangesNothing) {
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 0u));

  clock.reconcile(CalendarTime::epochMinutes(2000, 1, 1, 0, 0), 10'000u);
  uint32_t seconds = 0;
  ASSERT_TRUE(clock.now(10'000u, seconds));
  EXPECT_EQ(seconds, VALID_MINUTES * 60u + 10u) << "a broken RTC must not hijack a good anchor";
}

TEST(BookOrbitSessionClockTest, ReAnchoringNeverRewritesTimestampsAlreadyTaken) {
  // Stability is what the server's (user, book, device, page, startTime) dedup key rests on: a
  // stamp handed out before a re-anchor must keep its value, so a retry re-sends it unchanged.
  BookOrbitSessionClock clock;
  ASSERT_TRUE(clock.anchor(VALID_MINUTES, 0u));

  uint32_t captured = 0;
  ASSERT_TRUE(clock.now(30'000u, captured));

  clock.reconcile(VALID_MINUTES + 60, 31'000u);  // large jump: re-anchors
  uint32_t later = 0;
  ASSERT_TRUE(clock.now(31'000u, later));

  EXPECT_EQ(captured, VALID_MINUTES * 60u + 30u) << "the already-taken stamp is a value, not a view";
  EXPECT_GT(later, captured) << "only subsequent stamps move";
}

}  // namespace
