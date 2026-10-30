#include <gtest/gtest.h>

#include "RefreshEligibility.h"

TEST(RefreshEligibilityTest, PlainTextPageWithNoOtherFlagsIsEligible) {
  EXPECT_TRUE(isEligibleForNoFlashMaintenance(/*pageHasImages=*/false, /*needsTextGrayscale=*/false,
                                              /*manualRefreshPending=*/false,
                                              /*imageResidueCleanupPending=*/false));
}

TEST(RefreshEligibilityTest, PageWithImagesIsNeverEligible) {
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(/*pageHasImages=*/true, /*needsTextGrayscale=*/false,
                                               /*manualRefreshPending=*/false,
                                               /*imageResidueCleanupPending=*/false));
}

TEST(RefreshEligibilityTest, GrayscaleAaTextPageIsNotEligible) {
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(/*pageHasImages=*/false, /*needsTextGrayscale=*/true,
                                               /*manualRefreshPending=*/false,
                                               /*imageResidueCleanupPending=*/false));
}

TEST(RefreshEligibilityTest, ManualForcedRefreshIsNeverEligible) {
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(/*pageHasImages=*/false, /*needsTextGrayscale=*/false,
                                               /*manualRefreshPending=*/true,
                                               /*imageResidueCleanupPending=*/false));
}

// Regression lock for bug #2190: the page immediately following an image page must still flash
// even though it is plain text with no other disqualifying flag set.
TEST(RefreshEligibilityTest, PageImmediatelyAfterAnImagePageIsNotEligibleDueToResidue) {
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(/*pageHasImages=*/false, /*needsTextGrayscale=*/false,
                                               /*manualRefreshPending=*/false,
                                               /*imageResidueCleanupPending=*/true));
}

TEST(RefreshEligibilityTest, AnyDisqualifyingFlagOverridesAnOtherwiseEligiblePage) {
  // All four combinations where exactly one flag is set must all be ineligible -- already covered
  // individually above; this locks in that they compose correctly (no flag masks another).
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(true, true, true, true));
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(true, false, false, false));
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(false, true, false, false));
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(false, false, true, false));
  EXPECT_FALSE(isEligibleForNoFlashMaintenance(false, false, false, true));
}

// mustForceRealMaintenance bounds how many consecutive maintenance boundaries in a row may be
// satisfied by the no-flash substitute, so a long run of content-eligible pages (e.g. a pure-text
// chapter) can't defer a real flash forever on a panel with no driver-level ghost-clear safety net.
TEST(RefreshEligibilityTest, BelowTheBoundDoesNotForceARealFlush) {
  for (int count = 0; count < kMaxConsecutiveNoFlashMaintenance; count++) {
    EXPECT_FALSE(mustForceRealMaintenance(count)) << "count=" << count;
  }
}

TEST(RefreshEligibilityTest, AtOrAboveTheBoundForcesARealFlush) {
  EXPECT_TRUE(mustForceRealMaintenance(kMaxConsecutiveNoFlashMaintenance));
  EXPECT_TRUE(mustForceRealMaintenance(kMaxConsecutiveNoFlashMaintenance + 1));
  EXPECT_TRUE(mustForceRealMaintenance(1000));
}

TEST(RefreshEligibilityTest, ZeroConsecutiveCountNeverForcesAFlush) {
  // Guards against an off-by-one that would force a flush on the very first maintenance boundary.
  EXPECT_FALSE(mustForceRealMaintenance(0));
}
