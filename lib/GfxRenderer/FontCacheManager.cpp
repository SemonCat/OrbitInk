#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::forceReleaseTransientCaches() {
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();

  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseTransientRenderCache();
  }
  releaseScanSlotCapacity();

  const uint32_t freeAfter = ESP.getFreeHeap();
  const uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
  LOG_INF("FCM", "forceReleaseTransientCaches: free %u -> %u (+%d), maxAlloc %u -> %u (+%d)", freeBefore, freeAfter,
          static_cast<int32_t>(freeAfter) - static_cast<int32_t>(freeBefore), maxAllocBefore, maxAllocAfter,
          static_cast<int32_t>(maxAllocAfter) - static_cast<int32_t>(maxAllocBefore));
}

void FontCacheManager::forceReleaseSleepOverlayCaches() {
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseTransientRenderCache();
    font->clearPersistentCache();
  }
  releaseScanSlotCapacity();
  LOG_INF("FCM", "sleep PNG cache release: free %u -> %u, maxAlloc %u -> %u", freeBefore, ESP.getFreeHeap(),
          maxAllocBefore, ESP.getMaxAllocHeap());
}

int FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask,
                                   CancellationCallback isCancelled, const void* cancellationContext) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask, false, isCancelled, cancellationContext);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return missed;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return 0;

  for (uint8_t i = 0; i < 4; i++) {
    if (isCancelled && isCancelled(cancellationContext)) return SdCardFont::PREWARM_CANCELLED;
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
  return 0;
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

FontCacheManager::ScanSlot* FontCacheManager::findOrCreateScanSlot(int fontId) {
  for (int i = 0; i < scanSlotCount_; i++) {
    if (scanSlots_[i].fontId == fontId) return &scanSlots_[i];
  }
  if (scanSlotCount_ >= kMaxScanSlots) {
    // Overflow: more than kMaxScanSlots distinct fonts in one page scan.
    // Not expected in practice (see header comment) -- drop this font from
    // prewarm batching for this page rather than growing unbounded state.
    LOG_DBG("FCM", "scan: font id %d dropped, all %d scan slots in use", fontId, kMaxScanSlots);
    return nullptr;
  }
  ScanSlot& slot = scanSlots_[scanSlotCount_++];
  slot.fontId = fontId;
  return &slot;
}

void FontCacheManager::resetScanSlots() {
  for (int i = 0; i < scanSlotCount_; i++) {
    scanSlots_[i].fontId = -1;
    scanSlots_[i].text.clear();
    // Retain capacity across ordinary page turns (avoids a realloc+copy on
    // every turn), but cap it: an unusually text-heavy page should not retain
    // an oversized scan buffer permanently.
    if (scanSlots_[i].text.capacity() > kScanSlotCapacityCap) {
      scanSlots_[i].text.shrink_to_fit();
    }
    memset(scanSlots_[i].styleCounts, 0, sizeof(scanSlots_[i].styleCounts));
  }
  scanSlotCount_ = 0;
}

void FontCacheManager::releaseScanSlotCapacity() {
  for (int i = 0; i < kMaxScanSlots; i++) {
    scanSlots_[i].text.clear();
    scanSlots_[i].text.shrink_to_fit();
  }
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  ScanSlot* slot = findOrCreateScanSlot(fontId);
  if (!slot) return;
  slot->text += text;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  slot->styleCounts[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->resetScanSlots();
}

bool FontCacheManager::PrewarmScope::endScanAndPrewarm(CancellationCallback isCancelled,
                                                       const void* cancellationContext) {
  if (!active_) return false;
  manager_->scanMode_ = ScanMode::None;

  bool cancelled = false;
  for (int i = 0; i < manager_->scanSlotCount_; i++) {
    FontCacheManager::ScanSlot& slot = manager_->scanSlots_[i];
    if (slot.text.empty()) continue;

    uint8_t styleMask = 0;
    for (uint8_t s = 0; s < 4; s++) {
      if (slot.styleCounts[s] > 0) styleMask |= (1 << s);
    }
    if (styleMask == 0) styleMask = 1;  // default to regular

    const int result =
        manager_->prewarmCache(slot.fontId, slot.text.c_str(), styleMask, isCancelled, cancellationContext);
    if (result == SdCardFont::PREWARM_CANCELLED) {
      cancelled = true;
      break;
    }
  }

  manager_->resetScanSlots();
  return !cancelled;
}

void FontCacheManager::PrewarmScope::cancel() {
  if (!active_) return;
  manager_->scanMode_ = ScanMode::None;
  manager_->resetScanSlots();
  manager_->clearCache();
  active_ = false;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanText_ is empty)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
