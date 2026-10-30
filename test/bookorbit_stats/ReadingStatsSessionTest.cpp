// Tests for the REAL src/activities/reader/ReadingStatsSession.h.
//
// This is the piece that makes the reader's session commit safe to call from two places. The
// reader ends a session either through onExit() or -- when manual KOSync is launched -- before
// that path releases epub/section to free heap for the TLS handshake, which happens BEFORE
// ActivityManager ever calls onExit(). Both call the finalizer unconditionally, so the latch is
// the only thing standing between "the session is committed once" and "it is counted twice".

#include <gtest/gtest.h>

#include "ReadingStatsSession.h"

namespace {

TEST(ReadingStatsSessionTest, FinalizesExactlyOnceHoweverManyCallersTry) {
  ReadingStatsSession session;
  EXPECT_TRUE(session.beginFinalize()) << "the first caller performs the commit";
  EXPECT_FALSE(session.beginFinalize()) << "manual sync finalized first; onExit must not redo it";
  EXPECT_FALSE(session.beginFinalize());
}

TEST(ReadingStatsSessionTest, ResetStartsAFreshSession) {
  ReadingStatsSession session;
  ASSERT_TRUE(session.beginFinalize());
  session.reset();
  EXPECT_TRUE(session.beginFinalize()) << "opening the next book must be able to commit again";
}

TEST(ReadingStatsSessionTest, AQualifyingSessionIsCountedOnceInBothAggregates) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsSession::applySessionTotals(120, book, global);

  EXPECT_EQ(book.sessionCount, 1);
  EXPECT_EQ(global.totalSessions, 1u);
  EXPECT_EQ(book.totalReadingSeconds, 120u);
  EXPECT_EQ(global.totalReadingSeconds, 120u);
}

TEST(ReadingStatsSessionTest, ASessionBelowTheThresholdIsNotCounted) {
  // Opening a book to check one word should not inflate the session count.
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsSession::applySessionTotals(ReadingStatsSession::MIN_SESSION_SECONDS - 1, book, global);

  EXPECT_EQ(book.sessionCount, 0);
  EXPECT_EQ(global.totalSessions, 0u);
  EXPECT_EQ(book.totalReadingSeconds, 0u);
}

TEST(ReadingStatsSessionTest, TheThresholdItselfCounts) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsSession::applySessionTotals(ReadingStatsSession::MIN_SESSION_SECONDS, book, global);
  EXPECT_EQ(book.sessionCount, 1);
}

TEST(ReadingStatsSessionTest, DoubleApplicationWouldDoubleCountWhichIsWhatTheLatchPrevents) {
  // Documents the failure the latch exists to stop: applySessionTotals is deliberately NOT
  // self-limiting, so the ordering guarantee has to come from beginFinalize().
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsSession::applySessionTotals(120, book, global);
  ReadingStatsSession::applySessionTotals(120, book, global);
  EXPECT_EQ(book.sessionCount, 2) << "unguarded double application double counts, as expected";

  // With the latch in front of it, the second attempt never reaches the aggregate.
  BookReadingStats guardedBook;
  GlobalReadingStats guardedGlobal;
  ReadingStatsSession guarded;
  for (int i = 0; i < 2; i++) {
    if (guarded.beginFinalize()) ReadingStatsSession::applySessionTotals(120, guardedBook, guardedGlobal);
  }
  EXPECT_EQ(guardedBook.sessionCount, 1);
  EXPECT_EQ(guardedGlobal.totalReadingSeconds, 120u);
}

TEST(ReadingStatsSessionTest, AccumulationSaturatesRatherThanWrapping) {
  BookReadingStats book;
  GlobalReadingStats global;
  book.totalReadingSeconds = UINT32_MAX - 10;
  global.totalReadingSeconds = UINT32_MAX - 10;
  ReadingStatsSession::applySessionTotals(1000, book, global);
  EXPECT_EQ(book.totalReadingSeconds, UINT32_MAX);
  EXPECT_EQ(global.totalReadingSeconds, UINT32_MAX);
}

}  // namespace
