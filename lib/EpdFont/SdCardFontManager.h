#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;
struct SdCardFontFileInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the family's .cpfont at `pointSize`, or the nearest size it ships if
  // that exact size is not installed. Only one .cpfont file is loaded; other
  // sizes remain on disk. This keeps resident interval + kern/ligature tables to
  // one size's worth of memory. Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Additively load the .cpfont of `family` at the exact physical `pointSize`
  // (used for size-matched CJK UI fallback alongside the reader-size font).
  // Does not unload anything. If a font of that size is already loaded its id
  // is reused. Returns the font id, or 0 if the family has no file at that size
  // or loading failed.
  int loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Replace ONLY the reader-size font (loaded_.front()) with this family's file at `pointSize`,
  // leaving every additively-loaded UI fallback size (see loadFamilyExtraSize) resident and
  // registered. For the common "same family, different reading size" change, where unloadAll() +
  // loadFamily() + setupUiFallbacks() re-read an unchanged UI font off SD and rebuilt its
  // interval table for nothing -- tens of KB of allocate/free churn against a heap with no
  // PSRAM to absorb it.
  //
  // Preconditions the CALLER must establish (this class cannot see them):
  //   * the family is already loaded (currentFamilyName() == family.name);
  //   * the currently loaded reader size is NOT one of the UI fallback sizes, and neither is the
  //     requested one. Either would mean the reader entry and a UI fallback are the same
  //     LoadedFont (loadFamilyExtraSize dedupes by size), so replacing "the reader font" would
  //     free a font the fallback map still points at, or leave the reader entry somewhere other
  //     than loaded_.front() where getFontId() expects it.
  // Falling back to loadFamily() for those cases is correct and costs only the reload this
  // method exists to avoid.
  //
  // Returns false if the family has no file for `pointSize` or the load fails; on failure
  // everything is unloaded, matching loadFamily()'s failure state so callers can share one
  // error path.
  bool reloadReaderSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded.
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

  // Load+register a single .cpfont file and append it to loaded_.
  // Returns the font id, or 0 on failure (allocation, read, or id collision).
  int loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer);

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};
