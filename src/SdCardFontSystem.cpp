#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "CustomZhTwProfile.h"
#include "ReaderFontSizes.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
#if CROSSFADE_CUSTOM_ZH_TW
// X3 zh-TW custom profile: all three builtin UI roles (small/author lines,
// list rows, book-cover titles/headers) get a CJK fallback -- never trade
// away UI coverage -- but they all point at the SAME loaded 14pt SD font,
// not three separately-loaded sizes. SdCardFontManager::loadFamilyExtraSize()
// dedupes by point size (see its "Reuse an already-loaded font of the same
// size" comment): the first of these three calls loads the file once, the
// second and third find it already in `loaded_` and return the same fontId
// for free, so setFallbackFont() ends up mapping SMALL_FONT_ID, UI_10_FONT_ID,
// and UI_12_FONT_ID all to that one instance. This is a deliberate,
// evidence-driven choice, not the graduated 8/10/12->10/12/14 plan originally
// sketched for this profile -- see the RAM rationale below and the
// static_assert enforcing it stays this way.
//
// RAM rationale (measured against the real corpus + the real
// GuanKiapTsingKhai font, see dist/x3-zh-tw/charset-stats.json and
// implementation-report.md): the font's own Han-block coverage is fragmented
// enough that the corpus-driven interval manifest cannot be merged below
// ~2,779 intervals per resident style without either dropping a required
// codepoint or having fontconvert_sdcard.py's own rasterizer silently
// re-split the interval anyway (SdCardFont.cpp's MAX_INTERVALS is a hard
// load-reject, not a soft target). At 6 bytes/interval that is ~16.7KB PER
// simultaneously-resident style. Three separately-sized fallbacks (10/12/14)
// would keep 4 styles resident by default (reader@28 + 3 fallback sizes,
// none matching 28) = ~66.7KB, exceeding the X3 framebuffer's own
// 52,272-byte footprint on a device with no PSRAM and no free-heap
// measurement to validate against. Sharing one size across all three roles
// keeps full glyph coverage while capping the worst case at 2 resident
// styles (~33.3KB, ~64% of the framebuffer) -- the RAM saving comes from
// sharing, not from dropping coverage. Revisit the shared size (or add a
// second one) once a real X3's free-heap headroom is measured -- see
// implementation-report.md's open questions.
//
// Trade-off this DOES keep, by choice, not oversight: small/author-line and
// list-row CJK text renders at the same (larger) 14pt fallback as titles,
// not at a size visually matched to their normally-smaller built-in Latin
// counterparts. That's a real visual mismatch in a fixed-height row sized
// for 8pt/10pt Latin text, not audited call-site-by-call-site here (only the
// two LyraCarouselTheme sites that already assumed a title-sized fallback
// were fixed to use the text-aware getLineHeight/getFontAscenderSize
// overloads -- see GfxRenderer.h). Full glyph coverage was the non-negotiable
// requirement; matching every role's line-height assumption was not asked
// for and is flagged as a follow-up, not silently declared solved.
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 14},
    {UI_10_FONT_ID, 14},
    {UI_12_FONT_ID, 14},
};
static_assert(kUiFontSizes[0].pointSize == kUiFontSizes[1].pointSize &&
                  kUiFontSizes[1].pointSize == kUiFontSizes[2].pointSize,
              "X3 zh-TW custom profile: all three builtin UI roles must share one SD fallback "
              "point size (see the RAM rationale above) -- if you're deliberately splitting them "
              "back into separate sizes, update the RAM accounting in the comment above and in "
              "implementation-report.md first, this assert is the tripwire for that decision.");
#else
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};
#endif

// True if `pointSize` is one of the sizes setupUiFallbacks() loads additively. When the reader
// font is at such a size, SdCardFontManager::loadFamilyExtraSize() has deduped it against the
// reader font, so the two roles share one LoadedFont -- see reloadReaderSize()'s preconditions.
bool isUiFallbackPointSize(const uint8_t pointSize) {
  for (const auto& ui : kUiFontSizes) {
    if (ui.pointSize == pointSize) return true;
  }
  return false;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  bool profileAlreadyLoaded = false;

#if CROSSFADE_CUSTOM_ZH_TW
  // Settings survive an upgrade from upstream, so compile-time member
  // initializers are not enough. Apply the private profile exactly once, and
  // only after every packaged size is visible on SD. This keeps a missing or
  // partially-copied font folder from switching the UI to a language the
  // built-in Latin fonts cannot render.
  uint32_t availableSizeMask = 0;
  const auto* customFamily = registry_.findFamily(CustomZhTwProfile::FONT_FAMILY);
  if (customFamily) {
    for (const auto& file : customFamily->files) {
      availableSizeMask |= CustomZhTwProfile::sizeBit(file.pointSize);
    }
  }
  if (CustomZhTwProfile::shouldApplyDefaults(SETTINGS.zhTwCustomProfileApplied != 0,
                                             availableSizeMask)) {
    // File names alone are not enough: prove the two resident files parse and
    // allocate before persisting a language that depends on them. A truncated
    // package therefore remains in the safe Latin UI instead of booting into
    // tofu and marking the migration complete forever.
    const bool readerLoaded = manager_.loadFamily(*customFamily, renderer,
                                                  CustomZhTwProfile::READER_POINT_SIZE);
    const int uiFontId = readerLoaded ? manager_.loadFamilyExtraSize(
                                            *customFamily, renderer, CustomZhTwProfile::UI_POINT_SIZE)
                                      : 0;
    if (readerLoaded && uiFontId != 0) {
      std::strncpy(SETTINGS.sdFontFamilyName, CustomZhTwProfile::FONT_FAMILY,
                   sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      SETTINGS.fontPointSize = CustomZhTwProfile::READER_POINT_SIZE;
      SETTINGS.language = static_cast<uint8_t>(Language::ZH_TW);
      SETTINGS.zhTwCustomProfileApplied = 1;
      setupUiFallbacks(renderer);
      SETTINGS.saveToFile();
      I18N.setLanguage(Language::ZH_TW);
      profileAlreadyLoaded = true;
      LOG_INF("SDFS", "Applied complete X3 zh-TW font profile");
    } else {
      manager_.unloadAll(renderer);
      LOG_ERR("SDFS", "X3 zh-TW font profile files failed validation; keeping safe language/font settings");
    }
  } else if (SETTINGS.zhTwCustomProfileApplied == 0) {
    LOG_ERR("SDFS", "X3 zh-TW font profile incomplete (mask=0x%08lx); keeping safe language/font settings",
            static_cast<unsigned long>(availableSizeMask));
  }
#endif

  // If user has a saved SD font selection, load it
  if (!profileAlreadyLoaded && SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      const uint8_t readerPointSize =
          snapReaderPointSize(&registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize);
      if (manager_.loadFamily(*family, renderer, readerPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

#if CROSSFADE_CUSTOM_ZH_TW
  // Existing devices already carry zhTwCustomProfileApplied, so a changed
  // member initializer alone would never reach them. Apply the latency profile
  // exactly once after the font profile is known-good, then preserve any later
  // user choice.
  if (CustomZhTwProfile::shouldApplyPerformanceDefaults(SETTINGS.zhTwCustomProfileApplied != 0,
                                                        SETTINGS.zhTwPerformanceProfileApplied != 0)) {
    SETTINGS.textAntiAliasing = CustomZhTwProfile::DEFAULT_TEXT_ANTI_ALIASING ? 1 : 0;
    SETTINGS.zhTwPerformanceProfileApplied = 1;
    SETTINGS.saveToFile();
    LOG_INF("SDFS", "Applied X3 zh-TW latency profile (text AA=%u)", SETTINGS.textAntiAliasing);
  }
  if (CustomZhTwProfile::shouldApplyHomeDefaults(SETTINGS.zhTwCustomProfileApplied != 0,
                                                 SETTINGS.zhTwHomeProfileApplied != 0)) {
    SETTINGS.uiTheme = CrossPointSettings::UI_THEME::BOOKORBIT;
    SETTINGS.zhTwHomeProfileApplied = 1;
    SETTINGS.saveToFile();
    UITheme::getInstance().reload();
    LOG_INF("SDFS", "Applied X3 zh-TW BookOrbit Home profile");
  }
  static_assert(CustomZhTwProfile::THEME_BOOKORBIT == CrossPointSettings::UI_THEME::BOOKORBIT &&
                    CustomZhTwProfile::THEME_JOURNAL == CrossPointSettings::UI_THEME::JOURNAL,
                "CustomZhTwProfile's persisted theme values must match CrossPointSettings::UI_THEME");
  {
    const auto journal = CustomZhTwProfile::journalMigration(SETTINGS.zhTwCustomProfileApplied != 0,
                                                             SETTINGS.zhTwJournalProfileApplied != 0, SETTINGS.uiTheme);
    if (journal.markApplied) {
      const bool themeChanged = journal.theme != SETTINGS.uiTheme;
      SETTINGS.uiTheme = journal.theme;
      SETTINGS.zhTwJournalProfileApplied = 1;
      SETTINGS.saveToFile();
      if (themeChanged) UITheme::getInstance().reload();
      LOG_INF("SDFS", "Applied X3 zh-TW Journal profile (theme %u)", SETTINGS.uiTheme);
    }
  }
#endif

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  const uint8_t wantedPointSize = snapReaderPointSize(&registry_, wantedFamily, SETTINGS.fontPointSize);

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(wantedPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");

    // Reading-size-only change within the same family: swap just the reader font and keep the
    // UI fallback size resident. Unloading everything re-read an unchanged UI font off SD and
    // rebuilt its interval table (~16.7KB for the zh-TW profile's 14pt) for no reason, on the
    // heap-tightest device in the range.
    //
    // Not taken when the registry was rediscovered: the files on disk may have been replaced,
    // so every loaded size has to be re-read, which is exactly what unloadAll() gives us. Also
    // not taken when either the old or the new reader size IS a UI fallback size -- there the
    // reader font and a fallback are the same object (loadFamilyExtraSize dedupes by size), and
    // the full reload is both correct and rare.
    if (!registryWasDirty && !isUiFallbackPointSize(manager_.currentPointSize()) && !isUiFallbackPointSize(wantedPt)) {
      if (manager_.reloadReaderSize(*family, renderer, wantedPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        // The fallback map still points at the untouched UI sizes, so nothing has to be
        // re-registered. Re-run anyway: it is a map lookup plus an already-loaded-size probe in
        // this state, and it keeps a single place responsible for the fallback wiring.
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Swapped reader size for %s, kept UI fallback sizes loaded", wantedFamily);
        return;
      }
      // reloadReaderSize() already unloaded everything on failure; fall through to the shared
      // "clear the family" handling below by way of the reload path.
      LOG_ERR("SDFS", "Failed to swap reader size for %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
      return;
    }
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, wantedPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}
