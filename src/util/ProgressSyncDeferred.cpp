#include "ProgressSyncDeferred.h"

#include <FsHelpers.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Epub/Section.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <memory>
#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "ProgressMapper.h"
#include "ProgressSyncNetwork.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/UITheme.h"

namespace ProgressSyncDeferred {
namespace {
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

std::optional<ReaderRenderSpec> mappingRenderSpec(GfxRenderer& renderer) {
  const auto previousOrientation = renderer.getOrientation();
  switch (SETTINGS.orientation) {
    case CrossPointSettings::PORTRAIT: renderer.setOrientation(GfxRenderer::Portrait); break;
    case CrossPointSettings::LANDSCAPE_CW: renderer.setOrientation(GfxRenderer::LandscapeClockwise); break;
    case CrossPointSettings::INVERTED: renderer.setOrientation(GfxRenderer::PortraitInverted); break;
    case CrossPointSettings::LANDSCAPE_CCW: renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise); break;
  }
  int top;
  int right;
  int bottom;
  int left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  right += SETTINGS.screenMargin;
  bottom += std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const int viewportWidth = renderer.getScreenWidth() - left - right;
  const int viewportHeight = renderer.getScreenHeight() - top - bottom;
  renderer.setOrientation(previousOrientation);
  if (viewportWidth <= 0 || viewportHeight <= 0 || viewportWidth > UINT16_MAX || viewportHeight > UINT16_MAX) {
    LOG_ERR("SyncDef", "Invalid viewport for deferred progress mapping: %dx%d", viewportWidth, viewportHeight);
    return std::nullopt;
  }
  return SETTINGS.readerRenderSpec(static_cast<uint16_t>(viewportWidth), static_cast<uint16_t>(viewportHeight));
}

bool persistClear(bool disableQuickResume = false, const DeferredProgress* applied = nullptr) {
  const DeferredProgress before = APP_STATE.deferredProgress;
  const bool oldQuickResumeEligible = APP_STATE.quickResumeSkipRenderEligible;
  const std::string oldResetPath = APP_STATE.bookOrbitResetPath;
  const std::string oldResetDocument = APP_STATE.bookOrbitResetDocument;
  const uint64_t oldResetTimestamp = APP_STATE.bookOrbitResetTimestamp;
  const bool oldResetAcknowledged = APP_STATE.bookOrbitResetAcknowledged;
  APP_STATE.deferredProgress.clear();
  if (disableQuickResume) APP_STATE.quickResumeSkipRenderEligible = false;
  if (applied && applied->provider == 2) {
    if (applied->bookOrbitReset) {
      APP_STATE.bookOrbitResetPath = applied->path;
      APP_STATE.bookOrbitResetDocument = applied->document;
      APP_STATE.bookOrbitResetTimestamp = applied->timestamp;
      APP_STATE.bookOrbitResetAcknowledged = false;
    } else if (APP_STATE.bookOrbitResetPath == applied->path &&
               APP_STATE.bookOrbitResetDocument == applied->document) {
      APP_STATE.bookOrbitResetPath.clear();
      APP_STATE.bookOrbitResetDocument.clear();
      APP_STATE.bookOrbitResetTimestamp = 0;
      APP_STATE.bookOrbitResetAcknowledged = false;
    }
  }
  if (APP_STATE.saveToFile()) return true;
  APP_STATE.deferredProgress = before;
  APP_STATE.quickResumeSkipRenderEligible = oldQuickResumeEligible;
  APP_STATE.bookOrbitResetPath = oldResetPath;
  APP_STATE.bookOrbitResetDocument = oldResetDocument;
  APP_STATE.bookOrbitResetTimestamp = oldResetTimestamp;
  APP_STATE.bookOrbitResetAcknowledged = oldResetAcknowledged;
  LOG_ERR("SyncDef", "Could not clear deferred progress state");
  return false;
}

std::string expectedDocument(const DeferredProgress& deferred) {
  if (deferred.provider == 1) {
    return KOReaderDocumentId::calculateForProgress(
        deferred.path, KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME);
  }
  if (deferred.provider == 2) return ProgressSyncNetwork::binaryDocument(deferred.path);
  return {};
}
}  // namespace

bool stage(const DeferredProgress& candidate) {
  if (!candidate.valid()) {
    LOG_ERR("SyncDef", "Refusing invalid deferred remote position");
    return false;
  }

  const DeferredProgress before = APP_STATE.deferredProgress;
  if (!APP_STATE.deferredProgress.consider(candidate)) {
    return APP_STATE.deferredProgress.covers(candidate);
  }

  const bool oldQuickResumeEligible = APP_STATE.quickResumeSkipRenderEligible;
  APP_STATE.quickResumeSkipRenderEligible = false;
  if (!APP_STATE.saveToFile()) {
    APP_STATE.deferredProgress = before;
    APP_STATE.quickResumeSkipRenderEligible = oldQuickResumeEligible;
    LOG_ERR("SyncDef", "Could not persist deferred remote position");
    return false;
  }

  // The retained pixels represent the old progress. Removing the file prevents even a transient
  // stale-page restore on the next X3 wake; the normal boot/render path remains the fallback.
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

bool pendingForBook(const std::string& path, DeferredProgress& out) {
  const auto& deferred = APP_STATE.deferredProgress;
  if (!deferred.forBook(path)) return false;
  out = deferred;
  return true;
}

std::optional<int> readerSpine(const std::shared_ptr<Epub>& epub) {
  const auto& deferred = APP_STATE.deferredProgress;
  if (!deferred.forBook(epub->getPath())) return std::nullopt;
  if (expectedDocument(deferred) != deferred.document) {
    persistClear();
    return std::nullopt;
  }
  int spine = 0, page = 0, pages = 0;
  uint64_t timestamp = 0;
  bool legacy = false;
  if (EpubReaderUtils::loadProgress(epub->getCachePath(), spine, page, pages, timestamp, legacy)) {
    if (timestamp > deferred.timestamp) {
      persistClear();
      return std::nullopt;
    }
    if (timestamp == 0 && !legacy) return std::nullopt;
  }
  if (deferred.hasRichPosition && deferred.richSpineIndex < epub->getSpineItemsCount()) {
    return deferred.richSpineIndex;
  }
  return ProgressMapper::targetSpine(epub, {deferred.xpath, deferred.percentage});
}

bool applyForBook(const std::string& path, GfxRenderer& renderer) {
  const DeferredProgress deferred = APP_STATE.deferredProgress;
  if (!deferred.forBook(path) || !FsHelpers::hasEpubExtension(path)) return false;

  const std::string document = expectedDocument(deferred);
  if (document.empty() || document != deferred.document) {
    LOG_ERR("SyncDef", "Discarding deferred progress after book/provider identity changed");
    persistClear();
    return false;
  }

  // Headless timer pulls only stage raw progress and never reach this function. Once a renderer is
  // initialized, release retained glyph transients before EPUB/layout work; every caller has
  // already destroyed its network Session, so TLS and the bounded layout sample never coexist.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();

  auto epub = std::make_shared<Epub>(path, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(/*buildIfMissing=*/true, /*skipLoadingCss=*/SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("SyncDef", "Could not load EPUB to apply deferred progress");
    return false;
  }
  if (epub->getSpineItemsCount() <= 0 || epub->getBookSize() == 0) {
    LOG_ERR("SyncDef", "Deferred EPUB has no usable spine/content");
    return false;
  }

  int localSpine = 0;
  int localPage = 0;
  int localPages = 0;
  uint64_t localTimestamp = 0;
  bool localLegacy = false;
  bool hasLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), localSpine, localPage, localPages,
                                                localTimestamp, localLegacy);
  hasLocal = hasLocal && localSpine >= 0 && localSpine < epub->getSpineItemsCount() && localPage >= 0 &&
             ((localPages > 0 && localPage < localPages) || (localPages == 0 && localPage == 0));
  if (hasLocal && localTimestamp == 0 && !localLegacy) {
    LOG_INF("SyncDef", "Holding deferred progress because chronology is ambiguous");
    return false;
  }
  if (hasLocal && localTimestamp > deferred.timestamp) {
    persistClear();
    return false;
  }

  std::optional<CrossPointPosition> mapped;
  const std::optional<ReaderRenderSpec> renderSpec = mappingRenderSpec(renderer);
  const SavedProgressPosition saved{deferred.xpath, deferred.percentage};
  const int target = deferred.hasRichPosition && deferred.richSpineIndex < epub->getSpineItemsCount()
                         ? deferred.richSpineIndex
                         : ProgressMapper::targetSpine(epub, saved);
  const bool chapterStart = !deferred.xpath.empty() && ProgressMapper::isChapterStart(epub, saved) &&
                            (!deferred.hasRichPosition || deferred.richPageNumber == 0) &&
                            target == ProgressMapper::targetSpine(epub, saved);
  // An interior anchor requires a verified complete denominator. The reader owns missing
  // pagination and drives it through its budgeted loop; retain the raw handoff meanwhile.
  if (!chapterStart) {
    Section section(epub, target, renderer);
    if (!renderSpec || !section.loadSectionFile(*renderSpec) || section.isPartial()) return false;
  }
  if (chapterStart) {
    mapped = ProgressMapper::toCrossPoint(epub, saved, renderer, -1, 0, 0,
                                          renderSpec ? &*renderSpec : nullptr);
  } else if (deferred.provider == 1 && deferred.hasRichPosition) {
    KOReaderRichPosition rich;
    rich.pctQ = deferred.richPctQ;
    rich.spineIndex = deferred.richSpineIndex;
    rich.pageNumber = deferred.richPageNumber;
    rich.totalPages = deferred.richTotalPages;
    if (deferred.richHasParagraphIndex) rich.paragraphIndex = deferred.richParagraphIndex;
    rich.xpath = deferred.xpath;
    mapped = ProgressMapper::fromRichPosition(epub, rich, renderer);
  }
  if (!mapped.has_value() && !deferred.xpath.empty()) {
    mapped = ProgressMapper::toCrossPoint(epub, SavedProgressPosition{deferred.xpath, deferred.percentage}, renderer,
                                          /*currentSpineIndex=*/-1, /*totalPagesInCurrentSpine=*/0,
                                          /*fallbackTotalPages=*/0, renderSpec ? &*renderSpec : nullptr);
  }
  if (!mapped.has_value() || mapped->spineIndex < 0 || mapped->spineIndex >= epub->getSpineItemsCount() ||
      (!chapterStart && mapped->totalPages <= 0) || mapped->pageNumber < 0 ||
      (mapped->totalPages > 0 && mapped->pageNumber >= mapped->totalPages)) {
    LOG_ERR("SyncDef", "Deferred remote position could not be mapped safely");
    return false;
  }

  if (hasLocal && localTimestamp == deferred.timestamp) {
    if (mapped->spineIndex == localSpine && mapped->pageNumber == localPage) {
      persistClear(/*disableQuickResume=*/true, &deferred);
      return false;
    }
    // Seconds-granularity tie: the fetched server position is canonical. Applying it once and
    // preserving the same timestamp prevents a local push/pull oscillation on the next GET.
  }

  if (!EpubReaderUtils::saveProgress(*epub, mapped->spineIndex, mapped->pageNumber, mapped->totalPages,
                                     deferred.timestamp)) {
    LOG_ERR("SyncDef", "Failed to save deferred remote progress");
    return false;
  }
  LOG_INF("SyncDef", "Applied deferred provider=%u progress %.4f to spine=%d page=%d/%d", deferred.provider,
          deferred.percentage, mapped->spineIndex, mapped->pageNumber, mapped->totalPages);
  persistClear(/*disableQuickResume=*/true, &deferred);
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

}  // namespace ProgressSyncDeferred
