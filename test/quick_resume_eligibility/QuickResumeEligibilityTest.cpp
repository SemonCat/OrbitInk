#include <gtest/gtest.h>

#include "QuickResumeEligibility.h"

TEST(QuickResumeEligibilityTest, AllConditionsSatisfiedIsEligible) {
  EXPECT_TRUE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                    /*sectionCacheIsPartial=*/false, /*pageNumber=*/3, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, DirtyBaseViewAtSleepIsNotEligible) {
  // A menu/popup/footnote overlay was on screen when the frame was captured -- the restored
  // pixels don't actually show the reading page.
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/false, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/3, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, SectionCacheMissIsNotEligible) {
  // No on-disk cache hit for the resume spine (e.g. settings/render spec changed since sleep).
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/false,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/3, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, PartialSectionCacheIsNotEligible) {
  // A partial cache has no background build armed to finish it if resumed without a render.
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/true, /*pageNumber=*/3, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, NegativePageNumberIsNotEligible) {
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/-1, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, PageNumberEqualToPageCountIsNotEligible) {
  // pageNumber must be a valid INDEX (< cachedPageCount), not merely non-negative.
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/10, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, PageNumberBeyondPageCountIsNotEligible) {
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/11, /*cachedPageCount=*/10));
}

TEST(QuickResumeEligibilityTest, ZeroCachedPageCountIsNeverEligible) {
  EXPECT_FALSE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                     /*sectionCacheIsPartial=*/false, /*pageNumber=*/0, /*cachedPageCount=*/0));
}

TEST(QuickResumeEligibilityTest, LastValidPageIndexIsEligible) {
  EXPECT_TRUE(isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, /*sectionCacheLoaded=*/true,
                                    /*sectionCacheIsPartial=*/false, /*pageNumber=*/9, /*cachedPageCount=*/10));
}
