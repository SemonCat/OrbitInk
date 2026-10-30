#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// Pure, host-testable core of the TXT reader's line-break search.
//
// The original search was O(n^2) in both allocations and measurement: it walked a candidate
// break position backwards one codepoint at a time and re-measured the WHOLE prefix at each
// step via `line.substr(0, breakPos)` + getTextAdvanceX(). For Latin text the first rfind(' ')
// usually lands close, so the cost stayed hidden; for Chinese TXT files -- long lines with no
// spaces at all -- every visual line re-measured the source line from byte 0 once per codepoint.
//
// The fix rests on one property of GfxRenderer's SD-card advance-table measurement path: it
// sums a per-codepoint fixed-point advance and snaps to pixels exactly once, at the end. Width
// is therefore additive in fixed-point, so every prefix width is available from a single
// forward scan. Nothing here changes what a prefix measures -- only how many times it is
// measured (see GfxRenderer::advanceTableFitPrefix for the preconditions that keep the fast
// path equivalent to the general one).
namespace textwrap {

// 4.4 fixed-point advance of one codepoint, or a negative value to signal "cannot measure this
// codepoint here" (the caller then abandons the fast path). A plain function pointer + context
// rather than std::function: this is called once per codepoint on the layout path, and
// std::function would heap-allocate its closure and add several KB of binary for the signature.
using AdvanceFpFn = int32_t (*)(void* ctx, uint32_t cp);

// Longest prefix of `line` (in bytes, always on a UTF-8 boundary) whose accumulated advance,
// snapped to pixels the same way GfxRenderer does, is <= maxWidthPx.
//
// `line` must be NUL-terminated at line.data()[line.size()] -- the same contract the
// `const char*` measurement API it replaces already carries. (A std::string always satisfies
// this; an arbitrary substring view does not.) The decoder stops at an embedded NUL exactly as
// a C-string measurement would.
//
// Returns false without touching `outFitBytes` if `advance` reported a negative advance for any
// codepoint: prefix widths are only monotonically non-decreasing when every advance is >= 0, and
// a backwards-moving prefix width would make this scan's answer differ from the exhaustive
// search it replaces.
inline bool longestFittingPrefix(const std::string_view line, const int maxWidthPx, const AdvanceFpFn advance,
                                 void* ctx, size_t& outFitBytes) {
  // fp4::toPixel is (fp + 8) >> 4, i.e. round-to-nearest. Rather than re-deriving that here,
  // work out the largest fixed-point sum that still snaps to <= maxWidthPx: it is
  // (maxWidthPx << 4) + 7, since (v + 8) >> 4 <= maxWidthPx  <=>  v <= (maxWidthPx << 4) + 7.
  // Comparing in fixed-point keeps the whole scan integer and branch-light.
  if (maxWidthPx < 0) {
    outFitBytes = 0;
    return true;
  }
  const int64_t maxWidthFp = (static_cast<int64_t>(maxWidthPx) << 4) + 7;

  int64_t widthFp = 0;
  size_t fitBytes = 0;
  const auto* cursor = reinterpret_cast<const unsigned char*>(line.data());
  const auto* const end = cursor + line.size();

  while (cursor < end) {
    const unsigned char* const cpStart = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;  // embedded NUL: measurement (a C string API) would stop here too
    // A malformed trailing sequence can leave the cursor unmoved; bail rather than spin.
    if (cursor <= cpStart) break;

    const int32_t advFp = advance(ctx, cp);
    if (advFp < 0) return false;
    widthFp += advFp;
    if (widthFp > maxWidthFp) break;
    fitBytes = static_cast<size_t>(cursor - reinterpret_cast<const unsigned char*>(line.data()));
  }

  outFitBytes = fitBytes;
  return true;
}

// The break position the TXT reader's backwards search would have arrived at, given the longest
// fitting prefix. Exactly reproduces the original walk:
//   * it descends over space positions (rfind(' ')) while the prefix does not fit, so it lands
//     on the LARGEST space position that fits, if there is one above 0;
//   * with no such space it falls through to a codepoint-at-a-time descent, which lands on the
//     largest fitting codepoint boundary, i.e. `fitBytes`;
//   * a zero result is clamped to 1 -- a viewport too narrow for even one glyph still has to
//     consume a byte or the caller would not terminate. (Byte 1 can split a multi-byte
//     codepoint; that is pre-existing degenerate behavior, preserved deliberately.)
// `fitBytes` must be the value produced by longestFittingPrefix() for the same line and width.
inline size_t breakPosFromFit(const std::string_view line, const size_t fitBytes) {
  if (line.empty()) return 0;
  const size_t clamped = fitBytes < line.size() ? fitBytes : line.size();
  const size_t spacePos = line.rfind(' ', clamped);
  // rfind(' ', clamped) can return `clamped` itself; the original walk only ever adopted a space
  // strictly below its current candidate, but a space AT the fit boundary is also the largest
  // fitting space position, so both agree. Position 0 is excluded, matching `spacePos > 0`.
  if (spacePos != std::string_view::npos && spacePos > 0 && spacePos <= clamped) {
    return spacePos;
  }
  return clamped == 0 ? 1 : clamped;
}

}  // namespace textwrap
