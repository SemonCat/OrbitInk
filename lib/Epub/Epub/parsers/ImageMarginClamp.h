#pragma once
#include <cstdint>

// A full-viewport image (displayHeight already scaled down to fit the viewport by
// ChapterHtmlSlimParser's own scale-to-fit math) plus a nonzero CSS top margin can still push the
// image below the visible page: the scale-to-fit only bounds displayHeight <= viewportHeight, not
// currentY + marginTop + displayHeight together. This is most visible for the first (and only)
// element on a page -- currentPage->elements.empty() -- where no page-break check runs before the
// image is placed, and it recurs identically right after a forced page break, since the fresh
// page's fit is never re-verified against the same image.
//
// Clamps the top margin (never negative) so the image's bottom edge never exceeds the viewport,
// starting from `currentY` (the page's next-content Y before this margin is applied). Leaves
// marginTop untouched when the image already fits.
inline int16_t clampImageTopMargin(int currentY, int16_t marginTop, int displayHeight, int viewportHeight) {
  if (currentY + marginTop + displayHeight <= viewportHeight) return marginTop;
  const int available = viewportHeight - currentY - displayHeight;
  return available > 0 ? static_cast<int16_t>(available) : static_cast<int16_t>(0);
}
