#include <gtest/gtest.h>

#include <ProgressPullPolicy.h>
#include <ProgressRecord.h>
#include <ReadingPositionPolicy.h>

namespace {

using ProgressPullPolicy::DecisionInput;
using ProgressPullPolicy::RemoteState;
using ProgressPullPolicy::SyncAction;

constexpr uint64_t T1 = 1'800'000'001ULL;
constexpr uint64_t T2 = 1'800'000'002ULL;
constexpr uint64_t T3 = 1'800'000'003ULL;

TEST(ProgressPullPolicyTest, LatestLocalWinsEvenAtLowerPercentage) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T2;
  in.remoteState = RemoteState::Found;
  in.remoteTimestamp = T1;
  in.localSameAsRemote = false;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::PushLocal);
}

TEST(ProgressPullPolicyTest, LatestRemoteWinsEvenWhenItMovedBackwards) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T1;
  in.remoteState = RemoteState::Found;
  in.remoteTimestamp = T2;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::PullRemote);
}

TEST(ProgressPullPolicyTest, MissingRemotePushesOnlyKnownTimeLocal) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T1;
  in.remoteState = RemoteState::Missing;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::PushLocal);
  in.localTimestamp = 0;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::HoldUnknown);
  in.hasLocal = false;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::NoProgress);
}

TEST(ProgressPullPolicyTest, FailedOrInvalidGetNeverAllowsPut) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T2;
  in.remoteState = RemoteState::Unavailable;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::Abort);
  in.remoteState = RemoteState::Found;
  in.remoteTimestamp = 0;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::Abort);
  in.remoteTimestamp = 1'800'000'000'000ULL;  // milliseconds, not seconds
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::Abort);
}

TEST(ProgressPullPolicyTest, EqualTimestampUsesServerCanonicalPosition) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T1;
  in.remoteState = RemoteState::Found;
  in.remoteTimestamp = T1;
  in.localSameAsRemote = false;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::PullRemote);
  in.localSameAsRemote = true;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::AlreadySynced);
}

TEST(ProgressPullPolicyTest, DeferredNewestLowerPositionSuppressesStaleUpload) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T1;
  in.remoteState = RemoteState::Found;
  in.remoteTimestamp = T1;
  in.localSameAsRemote = true;
  in.hasDeferredRemote = true;
  in.deferredTimestamp = T3;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::HoldDeferred);

  in.remoteTimestamp = T3 + 1;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::PullRemote);
}

TEST(ProgressPullPolicyTest, EqualDeferredTieUsesCurrentServerCanonicalPosition) {
  DecisionInput in;
  in.hasLocal = true;
  in.localTimestamp = T1;
  in.remoteState = RemoteState::Missing;
  in.hasDeferredRemote = true;
  in.deferredTimestamp = T1;
  in.localSameAsDeferred = false;
  EXPECT_EQ(ProgressPullPolicy::decide(in), SyncAction::HoldDeferred);
}

TEST(ProgressPullPolicyTest, SequentialProvidersConvergeOnNewestOriginIndependentOfOrder) {
  auto apply = [](uint64_t& localTimestamp, uint64_t remoteTimestamp) {
    DecisionInput in;
    in.hasLocal = true;
    in.localTimestamp = localTimestamp;
    in.remoteState = RemoteState::Found;
    in.remoteTimestamp = remoteTimestamp;
    if (ProgressPullPolicy::decide(in) == SyncAction::PullRemote) localTimestamp = remoteTimestamp;
  };
  uint64_t koFirst = T1;
  apply(koFirst, T3);
  apply(koFirst, T2);
  uint64_t boFirst = T1;
  apply(boFirst, T2);
  apply(boFirst, T3);
  EXPECT_EQ(koFirst, T3);
  EXPECT_EQ(boFirst, T3);
}

TEST(ProgressRecordTest, LegacyAndPaginationRecordsMigrateWithUnknownTime) {
  const uint8_t legacy[] = {7, 0, 9, 0};
  ProgressRecord::Value value;
  ASSERT_TRUE(ProgressRecord::decode(legacy, sizeof(legacy), value));
  EXPECT_EQ(value.spineIndex, 7);
  EXPECT_EQ(value.pageNumber, 9);
  EXPECT_EQ(value.pageCount, 0);
  EXPECT_EQ(value.timestamp, 0u);

  const uint8_t v2[] = {7, 0, 9, 0, 40, 0};
  ASSERT_TRUE(ProgressRecord::decode(v2, sizeof(v2), value));
  EXPECT_EQ(value.pageCount, 40);
  EXPECT_EQ(value.timestamp, 0u);
}

TEST(ProgressRecordTest, UnchangedLegacySaveStaysLegacyUntilValidServerBaselineArrives) {
  const uint8_t oldBytes[] = {7, 0, 9, 0, 40, 0};
  ProgressRecord::Value loaded;
  ASSERT_TRUE(ProgressRecord::decode(oldBytes, sizeof(oldBytes), loaded));
  ASSERT_TRUE(loaded.legacy);

  // Reader open/close/reflow may update the stored pagination, but no actual position event has
  // occurred. Preserve a 6-byte record instead of silently converting unknown time to v3.
  loaded.pageCount = 44;
  uint8_t unchangedSave[ProgressRecord::TIMESTAMPED_SIZE] = {};
  const size_t savedSize = ProgressRecord::encode(loaded, unchangedSave, /*preserveLegacy=*/true);
  EXPECT_EQ(savedSize, ProgressRecord::PAGINATION_SIZE);

  ProgressRecord::Value reloaded;
  ASSERT_TRUE(ProgressRecord::decode(unchangedSave, savedSize, reloaded));
  EXPECT_TRUE(reloaded.legacy);
  EXPECT_EQ(reloaded.spineIndex, 7);
  EXPECT_EQ(reloaded.pageNumber, 9);
  EXPECT_EQ(reloaded.pageCount, 44);
  EXPECT_EQ(reloaded.timestamp, 0u);
  EXPECT_EQ(ProgressPullPolicy::decideLegacy(RemoteState::Found), SyncAction::PullRemote);
  EXPECT_EQ(ProgressPullPolicy::decideLegacy(RemoteState::Missing), SyncAction::HoldUnknown);
}

TEST(ProgressRecordTest, TimestampSurvivesEncodeDecodeAndLayoutCountChanges) {
  ProgressRecord::Value original{4, 12, 50, T2};
  uint8_t bytes[ProgressRecord::TIMESTAMPED_SIZE];
  ProgressRecord::encode(original, bytes);
  ProgressRecord::Value loaded;
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), loaded));
  EXPECT_EQ(loaded.timestamp, T2);

  loaded.pageCount = 61;  // reflow/layout representation, not a reading event
  ProgressRecord::encode(loaded, bytes);
  ProgressRecord::Value reloaded;
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), reloaded));
  EXPECT_EQ(reloaded.timestamp, T2);
}

TEST(ProgressRecordTest, InvalidClockDegradesToUnknownWithoutLosingPosition) {
  ProgressRecord::Value original{4, 12, 50, 1'800'000'000'000ULL};
  uint8_t bytes[ProgressRecord::TIMESTAMPED_SIZE];
  ProgressRecord::encode(original, bytes);
  ProgressRecord::Value loaded;
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), loaded));
  EXPECT_EQ(loaded.spineIndex, 4);
  EXPECT_EQ(loaded.pageNumber, 12);
  EXPECT_EQ(loaded.timestamp, 0u);
}

TEST(ReadingPositionPolicyTest, OnlyActualReaderNavigationMintsAnEvent) {
  EXPECT_FALSE(ReadingPositionPolicy::shouldStamp(/*logicalPositionChanged=*/false,
                                                  /*navigationRequested=*/false,
                                                  /*inFootnote=*/false))
      << "unchanged save/open/close/retry must retain the origin time";
  EXPECT_FALSE(ReadingPositionPolicy::shouldStamp(/*logicalPositionChanged=*/false,
                                                  /*navigationRequested=*/true,
                                                  /*inFootnote=*/false))
      << "a no-op seek or layout-only page-count change is not a reading event";
  EXPECT_FALSE(ReadingPositionPolicy::shouldStamp(/*logicalPositionChanged=*/true,
                                                  /*navigationRequested=*/true,
                                                  /*inFootnote=*/true));
  EXPECT_TRUE(ReadingPositionPolicy::shouldStamp(/*logicalPositionChanged=*/true,
                                                 /*navigationRequested=*/true,
                                                 /*inFootnote=*/false))
      << "forward/backward page turns and confirmed percent/chapter/bookmark seeks are events";
}

TEST(ReadingPositionPolicyTest, EqualSecondIsValidButBackwardOrInvalidClockBecomesUnknown) {
  EXPECT_EQ(ReadingPositionPolicy::timestampAfterChange(T1, T2), T2);
  EXPECT_EQ(ReadingPositionPolicy::timestampAfterChange(T1, T1), T1);
  EXPECT_EQ(ReadingPositionPolicy::timestampAfterChange(T2, T1), 0u);
  EXPECT_EQ(ReadingPositionPolicy::timestampAfterChange(T1, 0), 0u);
  EXPECT_EQ(ReadingPositionPolicy::timestampAfterChange(T1, 1'800'000'000'000ULL), 0u);
}

TEST(ReadingPositionPolicyTest, RecoveringClockDoesNotRestampPreviouslyUnknownProgress) {
  ProgressRecord::Value local{0, 8, 20, ReadingPositionPolicy::timestampAfterChange(T1, 0)};
  uint8_t bytes[ProgressRecord::TIMESTAMPED_SIZE];
  ProgressRecord::encode(local, bytes);
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), local));
  DecisionInput input{true, local.timestamp, RemoteState::Found, T2, false};
  // A successful NTP recovery alone supplies no evidence about this old event.
  EXPECT_EQ(ProgressPullPolicy::decide(input), SyncAction::HoldUnknown);
  EXPECT_EQ(local.timestamp, 0u);
  // A genuine later BACKWARD turn has a trustworthy new origin time and may push.
  local.pageNumber = 7;
  local.timestamp = ReadingPositionPolicy::timestampAfterChange(local.timestamp, T3);
  ProgressRecord::encode(local, bytes);
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), local));
  input.localTimestamp = local.timestamp;
  EXPECT_EQ(ProgressPullPolicy::decide(input), SyncAction::PushLocal);
  EXPECT_EQ(local.pageNumber, 7u);
}

}  // namespace
