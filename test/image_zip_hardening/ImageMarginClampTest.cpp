// Exercises the REAL lib/Epub/Epub/parsers/ImageMarginClamp.h helper -- pure logic, header-only,
// no Epub/HalStorage/parser stack needed. Covers the fix: a full-viewport image (already scaled
// to fit within the viewport by ChapterHtmlSlimParser's own scale-to-fit math) plus a nonzero CSS
// top margin could push the image below the visible page, since the scale-to-fit only bounds
// displayHeight <= viewportHeight, not currentY + marginTop + displayHeight together.

#include <gtest/gtest.h>

#include "Epub/parsers/ImageMarginClamp.h"

TEST(ImageMarginClampTest, LeavesMarginUntouchedWhenTheImageAlreadyFits) {
  // 480 (currentY=0) + margin 20 + height 400 = 420 <= viewport 800: fits, no clamp needed.
  EXPECT_EQ(clampImageTopMargin(/*currentY=*/0, /*marginTop=*/20, /*displayHeight=*/400, /*viewportHeight=*/800), 20);
}

TEST(ImageMarginClampTest, ClampsMarginForAFullViewportImageWithNonzeroTopMargin) {
  // The exact bug scenario: image already scaled to fill the whole viewport height, plus a CSS
  // margin that would otherwise push its bottom edge past the visible page.
  const int16_t clamped = clampImageTopMargin(/*currentY=*/0, /*marginTop=*/30, /*displayHeight=*/800,
                                              /*viewportHeight=*/800);
  EXPECT_EQ(clamped, 0) << "no room at all for a top margin once the image already fills the viewport";
}

TEST(ImageMarginClampTest, ClampsToTheExactRemainingRoomWhenSomeIsAvailable) {
  // viewport 800, image 750 tall: only 50px of room total. A requested 30px margin fits inside
  // that (750+30=780<=800) so it is left untouched -- clamping only kicks in once margin+height
  // would exceed the viewport.
  EXPECT_EQ(clampImageTopMargin(0, 30, 750, 800), 30);
  // A requested 100px margin does NOT fit (750+100=850>800): clamp down to the 50px that remains.
  EXPECT_EQ(clampImageTopMargin(0, 100, 750, 800), 50);
}

TEST(ImageMarginClampTest, NeverReturnsANegativeMarginWhenTheImageAloneAlreadyOverflows) {
  // displayHeight itself already exceeds viewportHeight (shouldn't normally happen given the
  // scale-to-fit upstream, but the helper must still degrade to "no margin" rather than a
  // negative one that would shift the image further out of bounds).
  EXPECT_EQ(clampImageTopMargin(0, 10, 900, 800), 0);
}

TEST(ImageMarginClampTest, AccountsForNonzeroCurrentYNotJustDisplayHeight) {
  // Reproduces the after-a-forced-page-break case: currentY is 0 on the fresh page (matches the
  // full-viewport case above), but the helper must also clamp correctly when some content already
  // occupies the page before the image (currentY > 0).
  EXPECT_EQ(clampImageTopMargin(/*currentY=*/100, /*marginTop=*/50, /*displayHeight=*/700, /*viewportHeight=*/800), 0);
  EXPECT_EQ(clampImageTopMargin(/*currentY=*/100, /*marginTop=*/50, /*displayHeight=*/600, /*viewportHeight=*/800), 50);
}
