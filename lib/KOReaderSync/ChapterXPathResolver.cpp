#include "ChapterXPathResolver.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ChapterXPathResolverInternal.h"

// This file used to resolve outgoing KOReader XPaths (the ones this device PUSHES to a
// standard KOReader sync server) by streaming each chapter through Expat. Expat's own
// per-parser and per-parse memory overhead is fine in isolation, but on the X3 (no PSRAM)
// this generation runs immediately after a TLS handshake has already consumed most of free
// heap for the sync request itself -- field evidence: "[KOX] XML parse error: out of memory",
// "Min Free reached 5116 bytes". A few KB of additional heap demand at that exact moment is
// what Expat could not get.
//
// Replaced with a hand-rolled, streaming, BOUNDED tokenizer (lib/KOReaderSync/
// ChapterXPathResolverInternal.h -- split into a header so host tests can drive it directly
// with raw XHTML strings): every piece of state is a fixed-size array or scalar living on the
// CALLER'S STACK (these resolver objects are always constructed as plain locals below, never
// heap-allocated), not the heap. Stack space is a separate, per-task budget untouched by heap
// fragmentation, so this scan can run correctly even when the heap has almost nothing left. No
// dynamic allocation happens anywhere in the scan itself; the only allocation is the final short
// XPath std::string built once, after a match, exactly as before.
//
// This is a deliberately minimal tokenizer, not a general XML parser: no DTD/schema handling,
// only "ns:" prefix stripping (matching the previous stripPrefix() behavior), and best-effort
// (not spec-perfect) comment/CDATA/processing-instruction/doctype skipping -- sufficient for
// real-world EPUB chapter XHTML, which does not rely on any of those constructs inside <body>.
// Named-entity decoding reuses the existing lookupHtmlEntity() table (htmlEntities.h); numeric
// entities (&#NNN; / &#xHH;) are recognized directly since counting them only needs "this is
// one codepoint," not the decoded character itself -- KOReader XPath text offsets are visible
// CODEPOINT counts, never the underlying bytes/entities.
//
// Behavioral parity with the Expat implementation this replaces (not the separate, pre-existing
// ParagraphStreamer in ProgressMapper.cpp, which serves a different fallback and is untouched):
// tag-name comparisons are case-SENSITIVE (matching XHTML's own case-sensitivity and the prior
// std::string == comparisons here), and characters inside <script>/<style>/<head> are not
// specially excluded beyond the existing <body> gate, matching the prior classes' behavior
// exactly (that filtering exists only in ProgressMapper.cpp's separate implementation).
//
// Deterministic depth/cap overflow fallback: see ChapterXPathResolverInternal.h's
// AncestryRecordingScanner and LevelSiblingState comments.

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  koxpath_detail::BoundedXPathParagraphResolver resolver(paragraphIndex);
  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    if (resolver.wasDepthCapped()) {
      LOG_DBG("KOX", "Paragraph %u in spine %d exceeded ancestry depth cap; XPath truncated", paragraphIndex,
              spineIndex);
    }
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d -> %s", paragraphIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d", paragraphIndex, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  koxpath_detail::BoundedVisibleCharCounter counter;
  if (!epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  koxpath_detail::BoundedXPathProgressResolver resolver(targetVisibleChar);
  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    if (resolver.wasDepthCapped()) {
      LOG_DBG("KOX", "Progress %.3f in spine %d exceeded ancestry depth cap; XPath truncated", intraSpineProgress,
              spineIndex);
    }
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}
