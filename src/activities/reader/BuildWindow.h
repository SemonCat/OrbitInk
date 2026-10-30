#pragma once

// Pure decision logic for "does the background section build still have work the reader
// actually needs right now?" -- shared by EpubReaderActivity's build tick (which does the
// work) and its skipLoopDelay() (which decides whether to burn CPU waiting for it).
//
// Keeping these two in sync matters: skipLoopDelay() used to key off isBuilding() alone, so
// once the build had run ahead of the reader by BUILD_WINDOW_AHEAD pages the tick correctly
// stopped doing work while the loop kept spinning at full CPU speed doing nothing -- for as
// long as the reader stayed on that chapter. Both now ask the same question.
//
// Deliberately excludes the heap gate: buildTickHeapGate() mutates buildHeapPaused as a side
// effect and must only be evaluated at the tick site. skipLoopDelay() reads the cached
// buildHeapPaused instead, exactly as before.
namespace readerbuild {

// isPartial: a rebuild extending a previous session's partial. Its pageCount is pinned at the
// partial's watermark until the build passes it, so the window comparison would wrongly read
// "far enough ahead" and stall the build at 0 pages -- then the first turn past the watermark
// re-parses the whole chapter synchronously. Partials therefore always count as having work.
constexpr bool hasBuildWorkInWindow(const bool isBuilding, const bool isPartial, const int pageCount,
                                    const int currentPage, const int windowAhead) {
  if (!isBuilding) return false;
  if (isPartial) return true;
  return pageCount < currentPage + windowAhead;
}

}  // namespace readerbuild
