#include "CssCacheLayout.h"

namespace csscache {

Result validateRecords(const ByteSource& source, const size_t maxRules, const size_t maxSelectorLength,
                       uint16_t& outRuleCount, const size_t maxSelectorBytes) {
  uint16_t ruleCount = 0;
  if (source.available(source.ctx) < sizeof(ruleCount)) {
    return Result::Truncated;
  }
  if (!source.read(source.ctx, &ruleCount, sizeof(ruleCount))) {
    return Result::ReadError;
  }
  if (ruleCount > maxRules) {
    return Result::BadRuleCount;
  }

  size_t selectorBytes = 0;
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (source.available(source.ctx) < sizeof(selectorLen)) {
      return Result::Truncated;
    }
    if (!source.read(source.ctx, &selectorLen, sizeof(selectorLen))) {
      return Result::ReadError;
    }
    if (selectorLen == 0 || selectorLen > maxSelectorLength) {
      return Result::BadSelectorLength;
    }
    if (selectorBytes > maxSelectorBytes || selectorLen > maxSelectorBytes - selectorBytes) {
      return Result::BadSelectorPoolSize;
    }
    selectorBytes += selectorLen;
    // Selector bytes and the style payload are skipped, not decoded: this answers "is the file
    // usable?", and building the rule map is the loading path's job.
    const size_t recordRemainder = static_cast<size_t>(selectorLen) + RULE_STYLE_BYTES;
    if (source.available(source.ctx) < recordRemainder) {
      return Result::Truncated;
    }
    if (!source.skip(source.ctx, recordRemainder)) {
      return Result::ReadError;
    }
  }

  outRuleCount = ruleCount;
  return Result::Ok;
}

}  // namespace csscache
