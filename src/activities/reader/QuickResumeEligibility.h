#pragma once

// Whether a boot-time Quick Resume may skip re-rendering the current EPUB page and trust the
// already-restored sleep-frame pixels instead -- preparing cached section state for interaction
// and deferring per-page metadata (footnotes) until actually needed. Pure decision logic -- no
// I/O -- so it is host-testable in isolation. A false result always falls back to the existing
// full render; this function only ever gates the OPTIMIZATION, never correctness.
//
// Every condition must hold:
//  - cleanBaseViewAtSleep: no menu/popup/footnote overlay was on screen when the frame was
//    captured (otherwise the restored pixels don't actually show the reading page).
//  - sectionCacheLoaded && !sectionCacheIsPartial: the on-disk section cache for the resume spine
//    exists, byte-matches the CURRENT render spec (settings or a firmware update could have
//    changed it since sleep), and is fully built -- a partial cache has no background build armed
//    to finish it if the reader resumes silently without a render.
//  - pageNumber is within [0, cachedPageCount): the saved page is actually a page of that cache.
inline bool isQuickResumeEligible(bool cleanBaseViewAtSleep, bool sectionCacheLoaded, bool sectionCacheIsPartial,
                                  int pageNumber, int cachedPageCount) {
  return cleanBaseViewAtSleep && sectionCacheLoaded && !sectionCacheIsPartial && pageNumber >= 0 &&
         pageNumber < cachedPageCount;
}
