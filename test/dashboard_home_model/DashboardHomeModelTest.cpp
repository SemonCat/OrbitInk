// Book-selection semantics for the Dashboard Home card
// (src/activities/home/DashboardHomeModel.h). Pure decision logic, so the whole contract is
// checkable without a renderer, a HAL, or an SD card.

#include <gtest/gtest.h>

#include <vector>

#include "DashboardHomeModel.h"

namespace {

// Stands in for HomeActivity's "is this recent book's backing file still on the card?" probe.
bool validityProbe(const void* ctx, const int index) { return (*static_cast<const std::vector<bool>*>(ctx))[index]; }

std::vector<int> order(const dashboard::BrowseOrderInput& input, std::vector<bool> valid) {
  int out[32] = {};  // generous fixed buffer; the capacity the model may use is passed explicitly
  const int count = dashboard::buildBrowseOrder(input, validityProbe, &valid, out, 16);
  return std::vector<int>(out, out + count);
}

dashboard::BrowseOrderInput inputFor(const int recentCount, const int maxEntries = 6) {
  dashboard::BrowseOrderInput in;
  in.recentCount = recentCount;
  in.maxEntries = maxEntries;
  return in;
}

}  // namespace

TEST(DashboardBrowseOrder, EmptyRecentsAndNoPinYieldsEmptyState) { EXPECT_TRUE(order(inputFor(0), {}).empty()); }

TEST(DashboardBrowseOrder, WithoutAPinTheOrderIsRecentsMostRecentFirst) {
  EXPECT_EQ(order(inputFor(3), {true, true, true}), (std::vector<int>{0, 1, 2}));
}

TEST(DashboardBrowseOrder, ValidPinnedBookIsAlwaysTheFirstSlot) {
  auto in = inputFor(3);
  in.pinEnabled = true;
  in.pinnedValid = true;
  in.pinnedRecentIndex = 2;
  // The pin leads, and never appears a second time among the recents that follow it.
  EXPECT_EQ(order(in, {true, true, true}), (std::vector<int>{2, 0, 1}));
}

TEST(DashboardBrowseOrder, PinnedBookNotInRecentsUsesTheSentinelSlot) {
  auto in = inputFor(2);
  in.pinEnabled = true;
  in.pinnedValid = true;
  in.pinnedRecentIndex = -1;
  EXPECT_EQ(order(in, {true, true}), (std::vector<int>{dashboard::kPinnedSlot, 0, 1}));
}

TEST(DashboardBrowseOrder, DisabledPinIsIgnoredEvenWhenOneIsStored) {
  auto in = inputFor(2);
  in.pinEnabled = false;  // SETTINGS.pinBookToHome off
  in.pinnedValid = true;
  in.pinnedRecentIndex = 1;
  EXPECT_EQ(order(in, {true, true}), (std::vector<int>{0, 1}));
}

TEST(DashboardBrowseOrder, MissingPinnedFileDegradesToTheNextValidRecent) {
  auto in = inputFor(2);
  in.pinEnabled = true;
  in.pinnedValid = false;  // the pinned path no longer resolves
  in.pinnedRecentIndex = 1;
  EXPECT_EQ(order(in, {true, true}), (std::vector<int>{0, 1}));
}

TEST(DashboardBrowseOrder, MissingRecentsAreSkipped) {
  EXPECT_EQ(order(inputFor(4), {false, true, false, true}), (std::vector<int>{1, 3}));
}

TEST(DashboardBrowseOrder, EverythingMissingDegradesToTheEmptyState) {
  auto in = inputFor(3);
  in.pinEnabled = true;
  in.pinnedValid = false;
  EXPECT_TRUE(order(in, {false, false, false}).empty());
}

TEST(DashboardBrowseOrder, PinnedEntryItselfMissingFromRecentsIsNotEmitted) {
  // pinnedValid says the file exists, but the recents entry it maps to is flagged missing. The
  // probe wins -- one source of truth for "can this slot be opened".
  auto in = inputFor(2);
  in.pinEnabled = true;
  in.pinnedValid = true;
  in.pinnedRecentIndex = 0;
  EXPECT_EQ(order(in, {false, true}), (std::vector<int>{1}));
}

TEST(DashboardBrowseOrder, RespectsMaxEntries) {
  EXPECT_EQ(order(inputFor(5, /*maxEntries=*/2), {true, true, true, true, true}), (std::vector<int>{0, 1}));
}

TEST(DashboardBrowseOrder, RespectsMaxEntriesWithAPinTakingTheFirstSlot) {
  auto in = inputFor(5, /*maxEntries=*/3);
  in.pinEnabled = true;
  in.pinnedValid = true;
  in.pinnedRecentIndex = 4;
  EXPECT_EQ(order(in, {true, true, true, true, true}), (std::vector<int>{4, 0, 1}));
}

TEST(DashboardBrowseOrder, ZeroCapacityWritesNothing) {
  int out[1] = {};
  EXPECT_EQ(dashboard::buildBrowseOrder(inputFor(3), nullptr, nullptr, out, 0), 0);
  EXPECT_EQ(dashboard::buildBrowseOrder(inputFor(3), nullptr, nullptr, nullptr, 4), 0);
}

TEST(DashboardBrowseOrder, NullProbeTreatsEveryEntryAsValid) {
  int out[8] = {};
  EXPECT_EQ(dashboard::buildBrowseOrder(inputFor(3), nullptr, nullptr, out, 8), 3);
}

// --- Ephemeral browse stepping -----------------------------------------------------------------

TEST(DashboardBrowseIndex, WrapsForwardAndBackward) {
  EXPECT_EQ(dashboard::nextBrowseIndex(0, 3), 1);
  EXPECT_EQ(dashboard::nextBrowseIndex(2, 3), 0);
  EXPECT_EQ(dashboard::prevBrowseIndex(0, 3), 2);
  EXPECT_EQ(dashboard::prevBrowseIndex(2, 3), 1);
}

TEST(DashboardBrowseIndex, SingleOrEmptyListNeverMoves) {
  // A no-op here is what keeps Left/Right from costing a full-screen repaint of the same card.
  EXPECT_EQ(dashboard::nextBrowseIndex(0, 1), 0);
  EXPECT_EQ(dashboard::prevBrowseIndex(0, 1), 0);
  EXPECT_EQ(dashboard::nextBrowseIndex(0, 0), 0);
  EXPECT_EQ(dashboard::prevBrowseIndex(0, 0), 0);
}

TEST(DashboardBrowseIndex, ClampSurvivesAShrunkenOrder) {
  EXPECT_EQ(dashboard::clampBrowseIndex(5, 3), 2);
  EXPECT_EQ(dashboard::clampBrowseIndex(-1, 3), 0);
  EXPECT_EQ(dashboard::clampBrowseIndex(2, 0), 0);
}
