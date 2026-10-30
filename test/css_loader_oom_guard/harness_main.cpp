// Standalone -fno-exceptions regression harness for CssParser's checked flat storage.
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CssCacheLayout.h"
#include "CssParser.h"

extern size_t g_nothrowCalls;
extern size_t g_nothrowOutstanding;
extern size_t g_nothrowPeak;
void resetNothrowTracking(size_t failOrdinal);

namespace {
constexpr char kCachePathBase[] = "/test";
constexpr char kCacheFile[] = "/test/css_rules.cache";
constexpr char kCacheTmp[] = "/test/css_rules.cache.tmp";
constexpr uint16_t kRuleCount = 562;
constexpr uint16_t kDistinctStyles = 543;
constexpr size_t kSelectorBytes = 6424;

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "ASSERTION FAILED: %s\n", message);
  std::abort();
}

void require(const bool condition, const char* message) {
  if (!condition) fail(message);
}

void appendU16(std::vector<uint8_t>& output, const uint16_t value) {
  output.push_back(static_cast<uint8_t>(value));
  output.push_back(static_cast<uint8_t>(value >> 8));
}

void appendU32(std::vector<uint8_t>& output, const uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}

void appendFloat(std::vector<uint8_t>& output, const float value) {
  uint8_t bytes[sizeof(value)];
  std::memcpy(bytes, &value, sizeof(value));
  output.insert(output.end(), bytes, bytes + sizeof(value));
}

void appendStyle(std::vector<uint8_t>& output, const uint16_t styleIndex) {
  const size_t before = output.size();
  output.push_back(static_cast<uint8_t>(CssTextAlign::Left));
  output.push_back(static_cast<uint8_t>(CssFontStyle::Normal));
  output.push_back(static_cast<uint8_t>(CssFontWeight::Normal));
  output.push_back(static_cast<uint8_t>(CssTextDecoration::None));
  output.push_back(static_cast<uint8_t>(CssTextDirection::Ltr));
  for (size_t i = 0; i < 11; ++i) {
    appendFloat(output, i == 1 ? static_cast<float>(styleIndex + 1) : 0.0f);
    output.push_back(static_cast<uint8_t>(i == 1 ? CssUnit::Pixels : CssUnit::Pixels));
  }
  output.push_back(static_cast<uint8_t>(CssDisplay::Block));
  output.push_back(static_cast<uint8_t>(CssVerticalAlign::Baseline));
  appendU32(output, (1u << 0) | (1u << 5));
  require(output.size() - before == csscache::RULE_STYLE_BYTES, "style payload width drifted");
}

std::string selectorFor(const uint16_t index) {
  char prefix[16];
  std::snprintf(prefix, sizeof(prefix), ".s%04u", index);
  std::string selector(prefix);
  const size_t targetLength = index < 242 ? 12 : 11;
  selector.append(targetLength - selector.size(), 'x');
  return selector;
}

std::vector<uint8_t> buildCache(const uint8_t version = CssParser::CSS_CACHE_VERSION) {
  std::vector<uint8_t> output;
  output.push_back(version);
  appendU16(output, kRuleCount);
  size_t selectorBytes = 0;
  for (uint16_t i = 0; i < kRuleCount; ++i) {
    const std::string selector = selectorFor(i);
    selectorBytes += selector.size();
    appendU16(output, static_cast<uint16_t>(selector.size()));
    output.insert(output.end(), selector.begin(), selector.end());
    appendStyle(output, i % kDistinctStyles);
  }
  require(selectorBytes == kSelectorBytes, "synthetic selector total is not the real-book shape");
  return output;
}

std::string buildCss() {
  std::string css;
  css.reserve(24 * kRuleCount);
  for (uint16_t i = 0; i < kRuleCount; ++i) {
    css += selectorFor(i);
    css += "{margin-top:";
    css += std::to_string(i % kDistinctStyles + 1);
    css += "px;}";
  }
  return css;
}

HalFile openRegistered(const char* path, const std::vector<uint8_t>& bytes) {
  HalStorage::registerTestFile(path, bytes);
  HalFile file;
  require(Storage.openFileForRead("TEST", path, file), "could not open registered input");
  return file;
}

HalFile openCss(const std::string& css) {
  return openRegistered("/test/input.css", std::vector<uint8_t>(css.begin(), css.end()));
}

void runColdFailure(const size_t failOrdinal) {
  HalStorage::resetTestFiles();
  const std::string css = buildCss();
  CssParser parser(kCachePathBase);
  HalFile source = openCss(css);
  resetNothrowTracking(failOrdinal);
  const CssParser::ParseResult failed = parser.loadFromStream(source);
  require(failed == CssParser::ParseResult::Partial, "cold injected failure was not Partial");
  require(!parser.saveToCache(), "partial cold parse was cacheable");
  require(!HalStorage::hasTestFile(kCacheFile), "partial cold parse created a cache");

  parser.clear();
  require(g_nothrowOutstanding == 0, "cold failure clear leaked tracked storage");
  source = openCss(css);
  resetNothrowTracking(0);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Complete, "cold retry did not complete");
  require(parser.ruleCount() == kRuleCount, "cold retry lost rules");
  require(parser.saveToCache(), "cold retry did not save a cache");
  parser.clear();
  require(g_nothrowOutstanding == 0, "cold retry clear leaked tracked storage");
}

void runWarmFailure(const size_t failOrdinal) {
  HalStorage::resetTestFiles();
  HalStorage::registerTestFile(kCacheFile, buildCache());
  CssParser parser(kCachePathBase);
  resetNothrowTracking(failOrdinal);
  require(parser.loadFromCache() == CssParser::CacheLoadResult::LowMemory,
          "warm injected failure was not LowMemory");
  require(parser.empty(), "warm injected failure left partial state");
  require(g_nothrowOutstanding == 0, "warm failure cleanup leaked tracked storage");

  resetNothrowTracking(0);
  require(parser.loadFromCache() == CssParser::CacheLoadResult::Complete, "warm retry did not complete");
  require(parser.ruleCount() == kRuleCount, "warm retry lost rules");
  parser.clear();
  require(g_nothrowOutstanding == 0, "warm retry clear leaked tracked storage");
}

size_t discoverColdAllocationCount() {
  HalStorage::resetTestFiles();
  const std::string css = buildCss();
  size_t count = 0;
  {
    CssParser parser(kCachePathBase);
    HalFile source = openCss(css);
    resetNothrowTracking(0);
    require(parser.loadFromStream(source) == CssParser::ParseResult::Complete,
            "cold allocation discovery did not complete");
    count = g_nothrowCalls;
  }
  require(g_nothrowOutstanding == 0, "cold parser destructor leaked tracked storage");
  return count;
}

size_t discoverWarmAllocationCount() {
  HalStorage::resetTestFiles();
  HalStorage::registerTestFile(kCacheFile, buildCache());
  size_t count = 0;
  {
    CssParser parser(kCachePathBase);
    resetNothrowTracking(0);
    require(parser.loadFromCache() == CssParser::CacheLoadResult::Complete,
            "warm allocation discovery did not complete");
    count = g_nothrowCalls;
  }
  require(g_nothrowOutstanding == 0, "warm parser destructor leaked tracked storage");
  return count;
}

void runAllocationSweep(const bool cold) {
  const size_t count = cold ? discoverColdAllocationCount() : discoverWarmAllocationCount();
  require(count >= 3, "allocation discovery missed a storage pool");
  for (size_t ordinal = 1; ordinal <= count; ++ordinal) {
    if (cold) runColdFailure(ordinal);
    else runWarmFailure(ordinal);
  }
  std::fprintf(stderr, "%s allocation sweep covered 1..%zu\n", cold ? "cold" : "warm", count);
}

void runBookShape() {
  HalStorage::resetTestFiles();
  const std::string css = buildCss();
  CssParser parser(kCachePathBase);
  HalFile source = openCss(css);
  resetNothrowTracking(0);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Complete, "book-shape cold parse failed");
  require(parser.ruleCount() == kRuleCount, "book-shape cold rule count mismatch");
  const size_t coldRetained = g_nothrowOutstanding;
  const size_t coldPeak = g_nothrowPeak;
  require(parser.saveToCache(), "book-shape cache save failed");
  parser.clear();
  require(g_nothrowOutstanding == 0, "clear did not release all rule storage");

  resetNothrowTracking(0);
  require(parser.loadFromCache() == CssParser::CacheLoadResult::Complete, "book-shape warm load failed");
  require(parser.ruleCount() == kRuleCount, "book-shape warm rule count mismatch");
  std::fprintf(stderr,
               "BOOK_SHAPE rules=%u styles=%u selector_bytes=%zu cold_retained=%zu cold_peak=%zu "
               "warm_retained=%zu warm_peak=%zu nothrow_calls=%zu sizeof_style=%zu\n",
               kRuleCount, kDistinctStyles, kSelectorBytes, coldRetained, coldPeak, g_nothrowOutstanding,
               g_nothrowPeak, g_nothrowCalls, sizeof(CssStyle));
}

void runReadFailure() {
  HalStorage::resetTestFiles();
  const std::string firstRule = ".a{margin-top:1px;}";
  const std::string css = firstRule + ".b{margin-top:2px;}";
  HalFile source = openCss(css);
  HalStorage::setReadFailure("/test/input.css", firstRule.size());
  CssParser parser(kCachePathBase);
  resetNothrowTracking(0);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Error,
          "read failure after a complete rule was reported successful");
  require(parser.ruleCount() == 1, "read-failure setup did not retain exactly one complete rule");
  require(!parser.saveToCache(), "read-failed parse was cacheable");
}

void runDeterministicLimits() {
  HalStorage::resetTestFiles();
  std::string css;
  for (size_t i = 0; i < 1600; ++i) css += ".r" + std::to_string(i) + "{text-align:center}";
  CssParser parser(kCachePathBase);
  HalFile source = openCss(css);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Limited, "rule cap was not deterministic");
  require(parser.ruleCount() == 1500, "bounded rules were lost");
  require(parser.saveToCache(), "bounded rules were not persisted");
  require(HalStorage::readTestFile(kCacheFile)[0] == CssParser::CSS_LIMITED_CACHE_VERSION,
          "degradation was not recorded");
  parser.clear();
  require(parser.validateCache() == CssParser::CacheStatus::Complete, "bounded cache did not validate");
  require(parser.loadFromCache() == CssParser::CacheLoadResult::Complete, "bounded cache did not reload");
  require(parser.ruleCount() == 1500, "bounded cache changed on warm load");
  parser.clear();
  parser.markLimited();  // Entire stylesheet skipped at the EPUB size gate.
  require(parser.saveToCache(), "empty deterministic fallback was not persisted");
  require(parser.loadFromCache() == CssParser::CacheLoadResult::Complete, "empty fallback was not usable");
  require(parser.empty(), "empty fallback acquired styles");
}

void runSemantics() {
  HalStorage::resetTestFiles();
  const std::string css =
      "P{text-align:left}.Foo{font-weight:bold}p.FOO{text-align:right}.foo{font-style:italic}"
      ".hidden{display:none!important}";
  CssParser parser(kCachePathBase);
  HalFile source = openCss(css);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Complete, "semantic parse failed");
  require(parser.ruleCount() == 4, "case-insensitive selector merge failed");
  const CssStyle resolved = parser.resolveStyle("p", "fOo");
  require(resolved.textAlign == CssTextAlign::Right, "composite selector cascade failed");
  require(resolved.fontWeight == CssFontWeight::Bold, "class style cascade failed");
  require(resolved.fontStyle == CssFontStyle::Italic, "case-insensitive merged declaration failed");
  require(parser.resolveStyle("div", "HIDDEN").display == CssDisplay::None, "hidden content semantics changed");
}

void runCacheIntegrity() {
  HalStorage::resetTestFiles();
  const std::vector<uint8_t> oldCache = buildCache();
  HalStorage::registerTestFile(kCacheFile, oldCache);
  const std::string css = ".a{margin-top:1px;}";
  CssParser parser(kCachePathBase);
  HalFile source = openCss(css);
  require(parser.loadFromStream(source) == CssParser::ParseResult::Complete, "integrity parse failed");
  HalStorage::setWriteFailure(kCacheTmp, 8);
  require(!parser.saveToCache(), "short cache write reported success");
  require(HalStorage::readTestFile(kCacheFile) == oldCache, "failed save overwrote the good cache");

  HalStorage::resetTestFiles();
  HalStorage::registerTestFile("/test/progress.bin", {1, 2, 3, 4});
  HalStorage::registerTestFile(kCacheFile, buildCache(9));
  CssParser oldVersion(kCachePathBase);
  require(oldVersion.validateCache() == CssParser::CacheStatus::Invalid, "v9 cache was not invalidated");
  require(!HalStorage::hasTestFile(kCacheFile), "v9 cache was not removed");
  require(HalStorage::hasTestFile("/test/progress.bin"), "CSS invalidation removed reading progress");

  auto truncated = buildCache();
  truncated.pop_back();
  HalStorage::registerTestFile(kCacheFile, truncated);
  require(oldVersion.validateCache() == CssParser::CacheStatus::Invalid, "truncated cache validated");

  auto malformed = buildCache();
  const size_t firstStyleOffset = 1 + 2 + 2 + selectorFor(0).size();
  malformed[firstStyleOffset] = 0xFF;
  HalStorage::registerTestFile(kCacheFile, malformed);
  require(oldVersion.loadFromCache() == CssParser::CacheLoadResult::Invalid, "malformed style cache loaded");
  require(oldVersion.empty(), "malformed cache left partial state");

  HalStorage::registerTestFile(kCacheFile, buildCache());
  require(oldVersion.validateCache() == CssParser::CacheStatus::Complete, "fresh v10 cache did not validate");
  require(oldVersion.loadFromCache() == CssParser::CacheLoadResult::Complete, "fresh v10 cache did not warm-load");
}
}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  const size_t ordinal = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
  if (mode == "cold-failure") runColdFailure(ordinal);
  else if (mode == "warm-failure") runWarmFailure(ordinal);
  else if (mode == "cold-sweep") runAllocationSweep(true);
  else if (mode == "warm-sweep") runAllocationSweep(false);
  else if (mode == "book-shape") runBookShape();
  else if (mode == "read-failure") runReadFailure();
  else if (mode == "deterministic-limits") runDeterministicLimits();
  else if (mode == "semantics") runSemantics();
  else if (mode == "cache-integrity") runCacheIntegrity();
  else {
    std::fprintf(stderr, "usage: %s cold-sweep | warm-sweep | cold-failure N | warm-failure N | book-shape | "
                         "read-failure | semantics | cache-integrity\n",
                 argv[0]);
    return 2;
  }
  return 0;
}
