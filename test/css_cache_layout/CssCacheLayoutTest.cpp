#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "CssCacheLayout.h"

namespace {

constexpr size_t kMaxRules = 1500;          // mirrors CssParser.cpp's MAX_RULES
constexpr size_t kMaxSelectorLength = 256;  // mirrors CssParser.cpp's MAX_SELECTOR_LENGTH

// In-memory stand-in for the cache file, positioned just past the version byte (where
// validateRecords starts).
struct Buffer {
  std::vector<uint8_t> bytes;
  size_t pos = 0;
};

bool bufRead(void* ctx, void* dst, const size_t n) {
  auto* b = static_cast<Buffer*>(ctx);
  if (b->pos + n > b->bytes.size()) return false;
  std::memcpy(dst, b->bytes.data() + b->pos, n);
  b->pos += n;
  return true;
}

bool bufSkip(void* ctx, const size_t n) {
  auto* b = static_cast<Buffer*>(ctx);
  if (b->pos + n > b->bytes.size()) return false;
  b->pos += n;
  return true;
}

size_t bufAvailable(void* ctx) {
  auto* b = static_cast<Buffer*>(ctx);
  return b->bytes.size() - b->pos;
}

csscache::ByteSource sourceFor(Buffer& buffer) { return {&buffer, &bufRead, &bufSkip, &bufAvailable}; }

void appendU16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

// Builds a record stream exactly as CssParser::saveToCache() lays one out (minus the version
// byte): rule count, then per rule a length-prefixed selector followed by RULE_STYLE_BYTES of
// opaque style payload.
std::vector<uint8_t> buildImage(const std::vector<std::string>& selectors) {
  std::vector<uint8_t> out;
  appendU16(out, static_cast<uint16_t>(selectors.size()));
  uint8_t filler = 0;
  for (const auto& selector : selectors) {
    appendU16(out, static_cast<uint16_t>(selector.size()));
    out.insert(out.end(), selector.begin(), selector.end());
    for (size_t i = 0; i < csscache::RULE_STYLE_BYTES; ++i) out.push_back(filler++);
  }
  return out;
}

csscache::Result validate(std::vector<uint8_t> image, uint16_t& outRuleCount) {
  Buffer buffer{std::move(image), 0};
  const auto source = sourceFor(buffer);
  return csscache::validateRecords(source, kMaxRules, kMaxSelectorLength, outRuleCount);
}

}  // namespace

// The stride check. Selector lengths deliberately differ per rule: with a wrong stride the first
// record still validates and the error only shows up as drift on later ones, so a single-rule
// test would pass against a broken constant.
TEST(CssCacheLayout, ValidatesMultipleRulesOfDifferingSelectorLength) {
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(buildImage({"p", "div.chapter-heading", "h1", "blockquote.pull", "a"}), ruleCount),
            csscache::Result::Ok);
  EXPECT_EQ(ruleCount, 5);
}

// The same records with one byte removed from the last payload must be rejected. If the stride
// were understated, the walk would finish early and wrongly accept this.
TEST(CssCacheLayout, RejectsAStreamOneByteShort) {
  auto image = buildImage({"p", "div.chapter-heading", "h1"});
  image.pop_back();
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::Truncated);
}

// ... and one extra byte at the end must NOT make it fail: trailing slack is not this walk's
// business, only that every declared record is fully present.
TEST(CssCacheLayout, TrailingBytesDoNotFailValidation) {
  auto image = buildImage({"p", "div.x"});
  image.push_back(0xAB);
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::Ok);
  EXPECT_EQ(ruleCount, 2);
}

TEST(CssCacheLayout, AcceptsAnEmptyRuleSet) {
  uint16_t ruleCount = 0xFFFF;
  EXPECT_EQ(validate(buildImage({}), ruleCount), csscache::Result::Ok);
  EXPECT_EQ(ruleCount, 0);
}

TEST(CssCacheLayout, RejectsARuleCountOverTheCap) {
  std::vector<uint8_t> image;
  appendU16(image, static_cast<uint16_t>(kMaxRules + 1));
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::BadRuleCount);
}

TEST(CssCacheLayout, RejectsAZeroLengthSelector) {
  auto image = buildImage({"p"});
  image[2] = 0;  // selector length low byte of the first record
  image[3] = 0;
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::BadSelectorLength);
}

TEST(CssCacheLayout, RejectsAnOverlongSelector) {
  auto image = buildImage({"p"});
  const auto tooLong = static_cast<uint16_t>(kMaxSelectorLength + 1);
  image[2] = static_cast<uint8_t>(tooLong & 0xFF);
  image[3] = static_cast<uint8_t>(tooLong >> 8);
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::BadSelectorLength);
}

TEST(CssCacheLayout, RejectsAggregateSelectorsOverPoolCap) {
  Buffer buffer{buildImage({"abcd", "efgh"}), 0};
  uint16_t ruleCount = 0;
  EXPECT_EQ(csscache::validateRecords(sourceFor(buffer), kMaxRules, kMaxSelectorLength, ruleCount, 7),
            csscache::Result::BadSelectorPoolSize);
}

// A file claiming rules it does not contain (the classic half-written cache) must be rejected
// rather than accepted on its header alone.
TEST(CssCacheLayout, RejectsARuleCountLargerThanTheContent) {
  auto image = buildImage({"p", "div"});
  image[0] = 5;  // claim 5 rules, only 2 present
  image[1] = 0;
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::Truncated);
}

TEST(CssCacheLayout, RejectsAnEmptyStream) {
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate({}, ruleCount), csscache::Result::Truncated);
  EXPECT_EQ(validate({0x01}, ruleCount), csscache::Result::Truncated);  // half a rule count
}

// A selector length header present but its bytes missing.
TEST(CssCacheLayout, RejectsARecordTruncatedInsideItsSelector) {
  std::vector<uint8_t> image;
  appendU16(image, 1);
  appendU16(image, 40);  // claims a 40-byte selector
  image.push_back('p');  // only one byte follows
  uint16_t ruleCount = 0;
  EXPECT_EQ(validate(image, ruleCount), csscache::Result::Truncated);
}

// Pins the byte count the whole scheme rests on. CssParser::loadFromCache() cross-checks the
// same number against what it actually reads at runtime.
TEST(CssCacheLayout, RuleStylePayloadSizeIsPinned) { EXPECT_EQ(csscache::RULE_STYLE_BYTES, 66u); }
