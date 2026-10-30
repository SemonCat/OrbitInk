#include <gtest/gtest.h>
#include <HalClock.h>
#include <ReadingPositionPolicy.h>
#include <ProgressPullPolicy.h>
#include <ProgressRecord.h>
#include <WiFi.h>
#include <esp_sntp.h>

namespace {
class X3ClockBoundary : public ::testing::Test {
 protected:
  void SetUp() override {
    halClock = HalClock{};
    Rtc::present = Rtc::readable = Rtc::writable = true;
    Rtc::value = Rtc::DateTime{};
    hostNtpRequests = hostNtpStatus = 0;
    hostMillis = 0;
    WiFi.connection = WL_CONNECTED;
  }
};

TEST_F(X3ClockBoundary, ReadableButBehindRtcIsValidYetNavigationAfterPullBecomesUnknown) {
  Rtc::value.year = 2024;
  halClock.begin();
  uint64_t now = 0;
  ASSERT_TRUE(halClock.getEpochSeconds(now));
  const uint64_t pulled = 1791072000;
  ASSERT_LT(now, pulled);
  // Production connect's September26 gate sees a successful epoch read, skips NTP.
  if (halClock.isAvailable() && !halClock.getEpochSeconds(now)) halClock.syncFromNTP();
  EXPECT_EQ(hostNtpRequests, 0);
  ProgressRecord::Value local{0, 7, 20, ReadingPositionPolicy::timestampAfterChange(pulled, now)};
  uint8_t bytes[ProgressRecord::TIMESTAMPED_SIZE];
  ProgressRecord::encode(local, bytes);
  ASSERT_TRUE(ProgressRecord::decode(bytes, sizeof(bytes), local));
  EXPECT_EQ(local.timestamp, 0u);
  EXPECT_EQ(ProgressPullPolicy::decide({true, local.timestamp, ProgressPullPolicy::RemoteState::Found,
                                       pulled, false}), ProgressPullPolicy::SyncAction::HoldUnknown);
}

TEST_F(X3ClockBoundary, InvalidRtcNtpTimeoutIsBoundedAndDoesNotRestampOfflinePosition) {
  Rtc::readable = false;
  halClock.begin();
  uint64_t now = 0;
  EXPECT_FALSE(halClock.getEpochSeconds(now));
  ProgressRecord::Value local{1, 3, 10, ReadingPositionPolicy::timestampAfterChange(1791072000, 0)};
  EXPECT_FALSE(halClock.syncFromNTP());
  EXPECT_EQ(hostMillis, 5000u);
  EXPECT_EQ(hostNtpRequests, 1);
  EXPECT_EQ(local.timestamp, 0u);
  EXPECT_EQ(local.pageNumber, 3u);
}

TEST_F(X3ClockBoundary, SuccessfulRecoveryOnlyAllowsSubsequentNewPositionEvents) {
  Rtc::readable = false;
  halClock.begin();
  ProgressRecord::Value local{1, 3, 10, 0};
  hostNtpStatus = SNTP_SYNC_STATUS_COMPLETED;
  ASSERT_TRUE(halClock.syncFromNTP());
  EXPECT_EQ(local.timestamp, 0u);
  uint64_t now = 0;
  ASSERT_TRUE(halClock.getEpochSeconds(now));
  local.pageNumber = 2;  // newest position goes backwards, not maximum percentage.
  local.timestamp = ReadingPositionPolicy::timestampAfterChange(local.timestamp, now);
  EXPECT_TRUE(ProgressTimestamp::valid(local.timestamp));
  EXPECT_EQ(ProgressPullPolicy::decide({true, local.timestamp, ProgressPullPolicy::RemoteState::Found,
                                       now - 1, false}), ProgressPullPolicy::SyncAction::PushLocal);
}

TEST_F(X3ClockBoundary, AbsentRtcCannotRecoverThroughTheCurrentHal) {
  Rtc::present = false;
  halClock.begin();
  uint64_t now = 0;
  EXPECT_FALSE(halClock.isAvailable());
  EXPECT_FALSE(halClock.getEpochSeconds(now));
  EXPECT_FALSE(halClock.syncFromNTP());
  EXPECT_EQ(hostNtpRequests, 0);
}
}
