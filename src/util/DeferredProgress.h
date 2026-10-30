#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <ProgressTimestamp.h>

struct DeferredProgress {
  uint8_t provider = 0;
  std::string path;
  std::string document;
  std::string xpath;
  float percentage = -1.0f;
  uint64_t timestamp = 0;
  bool bookOrbitReset = false;

  bool hasRichPosition = false;
  uint32_t richPctQ = 0;
  uint16_t richSpineIndex = 0;
  uint16_t richPageNumber = 0;
  uint16_t richTotalPages = 1;
  bool richHasParagraphIndex = false;
  uint16_t richParagraphIndex = 0;

  bool valid() const {
    const bool knownProvider = provider == 1 || provider == 2;
    const bool usablePosition = !xpath.empty() || (provider == 1 && hasRichPosition && richTotalPages > 0);
    const bool validRich = !hasRichPosition ||
                           (provider == 1 && richPctQ <= 1000000U && richTotalPages > 0 &&
                            richPageNumber < richTotalPages);
    return knownProvider && !path.empty() && document.size() == 32 && ProgressTimestamp::valid(timestamp) &&
           std::isfinite(percentage) &&
           percentage >= 0.0f && percentage <= 1.0f && usablePosition && validRich;
  }

  bool forBook(const std::string& book) const { return valid() && path == book; }

  void clear() { *this = DeferredProgress{}; }

  bool samePositionAs(const DeferredProgress& other) const {
    return xpath == other.xpath && bookOrbitReset == other.bookOrbitReset && hasRichPosition == other.hasRichPosition &&
           (!hasRichPosition || (richPctQ == other.richPctQ && richSpineIndex == other.richSpineIndex &&
                                 richPageNumber == other.richPageNumber && richTotalPages == other.richTotalPages &&
                                 richHasParagraphIndex == other.richHasParagraphIndex &&
                                 (!richHasParagraphIndex || richParagraphIndex == other.richParagraphIndex)));
  }

  bool covers(const DeferredProgress& candidate) const {
    return forBook(candidate.path) &&
           (timestamp > candidate.timestamp || (timestamp == candidate.timestamp && samePositionAs(candidate)));
  }

  // Keep the newest durable candidate, even when it moved backwards. With equal seconds the most
  // recently fetched valid server candidate replaces a divergent staged position deterministically.
  bool consider(const DeferredProgress& candidate) {
    if (!candidate.valid()) return false;
    if (forBook(candidate.path)) {
      if (candidate.timestamp < timestamp) return false;
      if (candidate.timestamp == timestamp) {
        if (samePositionAs(candidate)) return false;
      }
    }
    *this = candidate;
    return true;
  }
};
