#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  using CancellationCallback = bool (*)(const void*);
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();

  // Force-release every loaded SD-card font's TRANSIENT render caches (mini
  // glyph/bitmap/kern arenas, on-demand overflow ring) regardless of current
  // free heap -- see SdCardFont::releaseTransientRenderCache() for exactly
  // what is and isn't freed. Coverage/index tables, metrics, persistent
  // kern/ligature tables, and the persistent advance-measurement cache
  // survive, so callers keep a fully usable font, just without its "bet on
  // the next page" arenas. Also clears the flash-font decompressor's page
  // cache for consistency (that cache already has no heap-floor retention of
  // its own, so this is a no-op change in behavior for it, included so a
  // single call covers every font-related transient allocation).
  //
  // Intended for call sites that need a specific amount of GUARANTEED free
  // contiguous heap right now and cannot rely on the normal render-path
  // retention bet resolving in their favor -- e.g. before a TLS handshake,
  // where a retained ~20KB CJK page mini-bitmap can be the entire difference
  // between the TLS heap gate passing or failing (see
  // lib/KOReaderSync/KOReaderSyncClient.cpp's MIN_FREE_FOR_TLS/
  // MIN_BLOCK_FOR_TLS -- this function does not touch that gate, it just
  // improves the odds of clearing it). Logs FreeHeap/MaxAlloc before and
  // after at LOG_INF so a device serial capture can confirm the release
  // actually ran and how much headroom it recovered.
  void forceReleaseTransientCaches();

  // Sleep-overlay PNG pressure only: also drop RAM-only advance tables.
  // Fonts retain coverage, metrics and kern/ligature data and can prewarm again.
  // Does not unload fonts or touch any SD/EPUB/Quick Resume state.
  void forceReleaseSleepOverlayCaches();

  int prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F,
                   CancellationCallback isCancelled = nullptr, const void* cancellationContext = nullptr);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    bool endScanAndPrewarm(CancellationCallback isCancelled = nullptr, const void* cancellationContext = nullptr);
    void cancel();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // Scan-mode text is batched per RESOLVED font id (drawText() resolves CJK
  // fallback before calling recordText(), so body text and status-bar text
  // that both land on the same SD-card font end up in the same slot and get
  // prewarmed together in one SD read). A fixed-size array is used instead of
  // a std::map: this is on the per-page-turn hot path, and map node alloc/free
  // churn is exactly the kind of heap fragmentation this codebase's SdCardFont
  // mini-cache comments warn against. In practice a page uses at most 2-3
  // distinct fonts (body + UI/status fallback + maybe an inline style font);
  // kMaxScanSlots covers that with headroom. If a page's scan ever needs a
  // 5th distinct font id, that font is simply not prewarmed for this page --
  // it falls back to today's on-demand load, i.e. graceful degradation, not
  // an error.
  static constexpr int kMaxScanSlots = 4;
  // Per-slot text capacity is retained (not shrink_to_fit'd) across ordinary
  // page turns to avoid a realloc+copy on every single turn -- the baseline
  // single-buffer design already did this via one reserve(2048). Splitting
  // into kMaxScanSlots slots multiplies the worst-case retained buffer count,
  // so each slot's retained capacity is capped here: resetScanSlots() shrinks
  // any slot back down once its capacity exceeds this (an unusually
  // text-heavy page, e.g. a huge multi-style chapter), so one oversized page
  // can't permanently inflate steady-state retained memory. Matches the
  // baseline's original single reserve(2048), so worst-case steady-state
  // retained total is still bounded at kMaxScanSlots * kScanSlotCapacityCap
  // (4 * 2KiB = 8KiB) instead of growing unbounded with page content.
  static constexpr size_t kScanSlotCapacityCap = 2048;
  struct ScanSlot {
    int fontId = -1;
    std::string text;
    uint32_t styleCounts[4] = {};
  };
  ScanSlot scanSlots_[kMaxScanSlots];
  int scanSlotCount_ = 0;

  // Returns the slot for fontId, creating one if there's room; returns nullptr
  // if fontId is new and all slots are already in use (see kMaxScanSlots doc).
  ScanSlot* findOrCreateScanSlot(int fontId);
  // Clears text and resets bookkeeping for the next scan. Caps any slot whose
  // capacity grew past kScanSlotCapacityCap back down (see field comment).
  void resetScanSlots();
  // Unconditionally frees ALL scan slot text capacity, regardless of size.
  // Called from forceReleaseTransientCaches()/forceReleaseSleepOverlayCaches()
  // so a TLS-handshake or sleep-overlay heap gate can reclaim these buffers
  // the same way it already reclaims SdCardFont's mini glyph/bitmap arenas --
  // otherwise up to kScanSlotCapacityCap bytes per slot would sit retained
  // and unreachable by those "guarantee free contiguous heap now" call sites.
  void releaseScanSlotCapacity();

  // Test-only observation seam: host tests need to prove the retained
  // std::string capacity in each scan slot is actually bounded/released, not
  // merely that a font stays usable afterward (usability alone can't tell a
  // capped/released slot apart from a leaked one -- both still prewarm fine).
  // No production code may use this; it exists solely so
  // test/sdcardfont_transient_release/SdCardFontReleaseTest.cpp can read
  // private state without exposing a capacity accessor on the real API.
  friend class FontCacheManagerTestAccess;
};
