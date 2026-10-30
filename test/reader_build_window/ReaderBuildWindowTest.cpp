#include <gtest/gtest.h>

#include "BuildWindow.h"

using readerbuild::hasBuildWorkInWindow;

namespace {
// Mirrors EpubReaderActivity::BUILD_WINDOW_AHEAD.
constexpr int kWindow = 5;
}  // namespace

// The regression this predicate exists for: the build has run far enough ahead of the reader
// that the build tick deliberately does nothing, yet isBuilding() is still true. Keying
// skipLoopDelay() off isBuilding() alone made the main loop spin at full CPU doing no work for
// as long as the reader stayed in that chapter.
TEST(ReaderBuildWindow, NoWorkWhenBuiltFarEnoughAhead) {
  EXPECT_FALSE(hasBuildWorkInWindow(/*isBuilding=*/true, /*isPartial=*/false, /*pageCount=*/50,
                                    /*currentPage=*/10, kWindow));
}

TEST(ReaderBuildWindow, WorkWhenReaderApproachesTheWatermark) {
  // Exactly at the window edge: pageCount == currentPage + window -> already far enough ahead.
  EXPECT_FALSE(hasBuildWorkInWindow(true, false, /*pageCount=*/15, /*currentPage=*/10, kWindow));
  // One page closer than the window -> the build must keep going.
  EXPECT_TRUE(hasBuildWorkInWindow(true, false, /*pageCount=*/14, /*currentPage=*/10, kWindow));
}

TEST(ReaderBuildWindow, NoWorkWhenNotBuilding) {
  // Even with the reader well past the built pages, a finished/absent build has nothing to do.
  EXPECT_FALSE(hasBuildWorkInWindow(/*isBuilding=*/false, /*isPartial=*/false, /*pageCount=*/0,
                                    /*currentPage=*/100, kWindow));
  EXPECT_FALSE(hasBuildWorkInWindow(/*isBuilding=*/false, /*isPartial=*/true, /*pageCount=*/0,
                                    /*currentPage=*/100, kWindow));
}

// A partial's pageCount is pinned at the previous session's watermark until the rebuild passes
// it, so the window comparison would read "far enough ahead" and stall the rebuild at 0 pages.
// Partials must keep ticking regardless of the window.
TEST(ReaderBuildWindow, PartialAlwaysHasWorkRegardlessOfWindow) {
  EXPECT_TRUE(hasBuildWorkInWindow(/*isBuilding=*/true, /*isPartial=*/true, /*pageCount=*/900,
                                   /*currentPage=*/0, kWindow));
}

// Catch-up: the reader is ahead of what has been laid out (a jump, or a resume deeper into the
// chapter). The build has to run whether or not it is a partial.
TEST(ReaderBuildWindow, WorkWhenReaderIsAheadOfTheBuild) {
  EXPECT_TRUE(hasBuildWorkInWindow(true, false, /*pageCount=*/3, /*currentPage=*/40, kWindow));
}

TEST(ReaderBuildWindow, WorkAtTheStartOfAFreshBuild) {
  EXPECT_TRUE(hasBuildWorkInWindow(true, false, /*pageCount=*/0, /*currentPage=*/0, kWindow));
}
