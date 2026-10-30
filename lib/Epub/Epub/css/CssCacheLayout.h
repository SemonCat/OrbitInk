#pragma once

#include <cstddef>
#include <cstdint>

// Structural layout of the CSS rules cache file, shared by the code that validates it and
// testable without a filesystem.
//
// The cache is: [uint8 version][uint16 ruleCount] then, per rule,
//   [uint16 selectorLen][selectorLen bytes][RULE_STYLE_BYTES of CssStyle payload].
//
// Split out because validating the file WITHOUT materializing every rule means seeking over the
// payloads by a fixed stride, and a stride that disagrees with what CssParser::saveToCache()
// writes would drift across records and reject a perfectly good cache -- which presents as a
// full CSS reparse on every warm book open, i.e. the opposite of the intended saving. Keeping
// the arithmetic here lets it be tested directly against a byte image.
namespace csscache {

// Serialized size of one rule's CssStyle payload -- everything saveToCache() writes for a rule
// after its length-prefixed selector:
//   5 enums (textAlign, fontStyle, fontWeight, textDecoration, direction)   =  5
//   + 11 CssLength, each a float value + a uint8 unit                      = 55
//   + display (uint8) + verticalAlign (uint8)                              =  2
//   + defined bits (uint32)                                                =  4
constexpr size_t RULE_STYLE_BYTES = 66;
static_assert(RULE_STYLE_BYTES ==
                  5 * sizeof(uint8_t) + 11 * (sizeof(float) + sizeof(uint8_t)) + 2 * sizeof(uint8_t) + sizeof(uint32_t),
              "CSS cache rule payload size drifted -- update it here and bump CSS_CACHE_VERSION");

// Sequential byte source, so the same validation runs over a HalFile and over an in-memory
// image. Plain function pointers + context rather than a virtual interface or std::function:
// this is firmware code and neither the vtable nor a heap-allocated closure earns its place.
struct ByteSource {
  void* ctx;
  // Reads exactly `n` bytes into `dst`; returns false if fewer were available.
  bool (*read)(void* ctx, void* dst, size_t n);
  // Advances `n` bytes without reading them.
  bool (*skip)(void* ctx, size_t n);
  // Bytes remaining.
  size_t (*available)(void* ctx);
};

enum class Result : uint8_t {
  Ok,
  Truncated,          // ran out of bytes mid-record
  BadRuleCount,         // rule count above the caller's cap
  BadSelectorLength,    // zero-length or over the caller's cap
  BadSelectorPoolSize,  // aggregate selector bytes exceed the caller's pool cap
  ReadError,            // the source failed a read/skip it had the bytes for
};

// Validates the record stream, starting at the rule-count field (the caller has already read and
// checked the version byte). Walks every record, checking the same shape constraints the loading
// path enforces, without decoding any selector or style. On success `outRuleCount` receives the
// declared rule count.
Result validateRecords(const ByteSource& source, size_t maxRules, size_t maxSelectorLength, uint16_t& outRuleCount,
                       size_t maxSelectorBytes = static_cast<size_t>(-1));

}  // namespace csscache
