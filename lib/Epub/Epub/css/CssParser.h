#pragma once

#include <HalStorage.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Storage is three flat, independently-growable buffers instead of a hash map:
 * a sorted array of tiny selector-entry records (binary-search lookup), a
 * single contiguous byte pool holding every selector's characters, and a
 * deduplicated pool of unique serialized style bodies (referenced by index, so
 * two selectors with the same declarations share one 66-byte copy). Styles use
 * fixed-size chunks to avoid CssStyle padding and old+new growth overlap. Every
 * growth site uses makeUniqueNoThrow, so allocation failure is reported through
 * a result enum instead of throwing past this file's -fno-exceptions boundary.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  // v10 invalidates v9 caches because older callers could save a successful-looking cache after
  // a stylesheet was skipped or only partly parsed. The record layout remains unchanged.
  static constexpr uint8_t CSS_CACHE_VERSION = 10;

  // Outcome of growing one of the internal storage pools.
  enum class PoolResult : uint8_t { Ready, Limit, OutOfMemory };

  // Outcome of inserting or merging one selector's declarations.
  enum class RuleInsertResult : uint8_t { Inserted, Merged, Limit, OutOfMemory };

  // Outcome of a full stylesheet parse. Partial means some rules were dropped (limit hit,
  // allocation failure, or the parser buffers truncated malformed/oversized input) -- callers
  // must not treat Partial as equivalent to a clean parse when deciding whether to trust a cache.
  enum class ParseResult : uint8_t { Complete, Partial, Error };

  // Outcome of loading rules from the on-disk cache into memory.
  enum class CacheLoadResult : uint8_t { Complete, LowMemory, Invalid };

  // Outcome of inspecting the on-disk cache without materializing it.
  enum class CacheStatus : uint8_t { Missing, Complete, Invalid };

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete if every rule in the stream was stored; Partial if the rule/selector/style
   *   limits were hit, an allocation failed mid-parse, or the input was truncated/malformed --
   *   the rules stored so far are still valid and usable, just not the full stylesheet; Error if
   *   the stream is invalid or a read fails, including after one or more complete rules.
   */
  ParseResult loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return entryCount_ == 0; }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return entryCount_; }

  /**
   * Clear all loaded rules and release the storage pools back to the heap.
   */
  void clear();

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache files.
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if a complete accumulated parse was written successfully; false after any
   *   incomplete/error stream since the last clear(), or if writing/promotion fails.
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading (on any outcome other than Complete, the parser is
   * left empty rather than partially populated).
   * @return Complete if every cached rule was hydrated; LowMemory if a storage pool could not
   *   grow (the cache file itself was fine -- retry later once more heap is available, do not
   *   delete it); Invalid if the cache is missing, malformed, version-mismatched (the stale file
   *   is removed), or over the rule/selector caps (source stylesheets should be reparsed).
   */
  CacheLoadResult loadFromCache();

  /**
   * Check that the cache file is present, current, and structurally intact -- WITHOUT building
   * the rule store. Applies the same rejections loadFromCache() does (version mismatch, which also
   * removes the stale file; rule count over the cap; an out-of-range selector length; truncation
   * at any record boundary) by walking the records and seeking over their payloads.
   *
   * For the warm-open path, which only needs to know whether the cache is usable: it used to ask
   * by calling loadFromCache() and then clear()ing the result, materializing every rule (each with
   * a heap selector string) purely to throw it away. Section building reloads the rules from this
   * same cache when it actually needs them.
   *
   * Does not touch the in-memory rules either way -- unlike loadFromCache(), which clears them.
   * @return Complete if the cache is usable as-is; Missing if there is no cache file (or the
   *   cachePath is unset); Invalid if it is present but stale/malformed (a stale-version file is
   *   removed as a side effect, matching loadFromCache()'s behavior).
   */
  CacheStatus validateCache();

 private:
  // One stored selector: an offset+length slice into selectorPool_, plus the index of its
  // deduplicated style body in styleBlocks_. Kept sorted by selector bytes (ASCII-lowercased at
  // insertion) so lookups are a binary search instead of a hash. 8 bytes, so 1500 entries costs
  // 12KB -- see MAX_RULES/SELECTOR_POOL_CAP/MAX_UNIQUE_STYLES in CssParser.cpp.
  struct SelectorEntry {
    uint32_t offset;
    uint16_t styleIndex;
    uint16_t length;
  };
  static_assert(sizeof(SelectorEntry) == 8);

  std::unique_ptr<SelectorEntry[]> entries_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;

  std::unique_ptr<char[]> selectorPool_;
  uint32_t selectorPoolSize_ = 0;
  uint32_t selectorPoolCapacity_ = 0;

  static constexpr uint16_t STYLE_BLOCK_CAPACITY = 64;
  static constexpr uint16_t STYLE_BLOCK_COUNT = 10;
  static_assert(STYLE_BLOCK_CAPACITY * STYLE_BLOCK_COUNT == 640);
  std::array<std::unique_ptr<uint8_t[]>, STYLE_BLOCK_COUNT> styleBlocks_;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;

  // Set once ensureEntryCapacity/ensureSelectorPoolCapacity/ensureStyleCapacity reports Limit or
  // OutOfMemory during a parse. Once set, processRuleBlockWithStyle() stops attempting new
  // inserts (avoiding repeated failing allocations) but keeps merging declarations into selectors
  // that are already stored, so a stylesheet that overflows partway through still yields a usable
  // (if incomplete) style set rather than an arbitrarily truncated one.
  bool ruleGrowthStopped_ = false;

  // Sticky across multiple stylesheet streams. Any read error, truncation, malformed ending,
  // limit, or allocation failure makes the accumulated rule set ineligible for persistence even
  // if a later stylesheet parses successfully. clear() begins a fresh accumulation.
  bool cacheable_ = true;

  std::string cachePath;

  // Storage pool growth, each backed by makeUniqueNoThrow -- see CssParser.cpp.
  PoolResult ensureEntryCapacity(size_t needed);
  PoolResult ensureSelectorPoolCapacity(size_t needed);
  PoolResult ensureStyleCapacity(size_t needed);

  // Finds or creates the deduplicated style body matching `style`; writes its index to
  // indexOut. Styles are compared by their serialized wire form (see CssParser.cpp), so two
  // selectors with identical declarations always share one CssStyle.
  PoolResult internStyle(const CssStyle& style, uint16_t& indexOut);

  // Case-insensitive, three-piece comparison against the selector stored at `entry`
  // (concatenation of p0+p1+p2, without materializing it). Used by lowerBound() both for plain
  // selectors (p1/p2 empty) and composite lookups (e.g. {tagName, ".", cls}).
  int compareEntryToPieces(const SelectorEntry& entry, std::string_view p0, std::string_view p1,
                            std::string_view p2) const;

  // Binary search over entries_ for the (p0+p1+p2) key. `exact` reports whether the returned
  // index is an exact match; when false it is the insertion point that keeps entries_ sorted.
  size_t lowerBound(std::string_view p0, std::string_view p1, std::string_view p2, bool& exact) const;

  // Decodes the stored style for the selector into out; false when absent.
  bool findStyle(std::string_view p0, std::string_view p1, std::string_view p2, CssStyle& out) const;

  uint8_t* styleWireAt(size_t index);
  const uint8_t* styleWireAt(size_t index) const;

  std::string_view selectorAt(size_t index) const;

  // Inserts a new selector or merges `style` into an existing one (cascade: later declarations
  // for the same selector override earlier ones for properties both define). Every fallible
  // allocation site inside (entry array growth, selector pool growth, style interning) is
  // reported through the returned enum rather than throwing.
  RuleInsertResult insertOrMerge(std::string_view selector, const CssStyle& style);

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
