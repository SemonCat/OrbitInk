#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "KOSyncStatus.h"

TEST(KOSyncStatus, ConvertsCivilDateToEpochMinutes) {
  EXPECT_EQ(0u, KOSyncStatus::epochMinutes(1970, 1, 1, 0, 0));
  EXPECT_EQ(28401120u, KOSyncStatus::epochMinutes(2024, 1, 1, 0, 0));
  EXPECT_EQ(28486079u, KOSyncStatus::epochMinutes(2024, 2, 28, 23, 59));
  EXPECT_EQ(28486080u, KOSyncStatus::epochMinutes(2024, 2, 29, 0, 0));
}

TEST(KOSyncStatus, FormatsCompactRelativeAge) {
  std::array<char, 16> out{};
  const uint32_t base = KOSyncStatus::epochMinutes(2026, 9, 2, 12, 0);
  EXPECT_TRUE(KOSyncStatus::formatRelative(base + 5, base, out.data(), out.size()));
  EXPECT_STREQ("5m", out.data());
  EXPECT_TRUE(KOSyncStatus::formatRelative(base + 125, base, out.data(), out.size()));
  EXPECT_STREQ("2h", out.data());
  EXPECT_TRUE(KOSyncStatus::formatRelative(base + 3 * 1440, base, out.data(), out.size()));
  EXPECT_STREQ("3d", out.data());
}

TEST(KOSyncStatus, UsesMonthDayAfterOneWeek) {
  std::array<char, 16> out{};
  const uint32_t last = KOSyncStatus::epochMinutes(2026, 8, 20, 12, 0);
  const uint32_t now = KOSyncStatus::epochMinutes(2026, 9, 2, 12, 0);
  EXPECT_TRUE(KOSyncStatus::formatRelative(now, last, out.data(), out.size()));
  EXPECT_STREQ("08/20", out.data());
}

TEST(KOSyncStatus, HidesMissingOrFutureTimestamp) {
  std::array<char, 16> out{};
  EXPECT_FALSE(KOSyncStatus::formatRelative(100, 0, out.data(), out.size()));
  EXPECT_FALSE(KOSyncStatus::formatRelative(99, 100, out.data(), out.size()));
}
