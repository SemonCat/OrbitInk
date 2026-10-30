#include "BookOrbitSetupGuideActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitSettingsActivity.h"
#include "BookOrbitSetupPolicy.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/themes/journal/JournalText.h"
#include "fontIds.h"

namespace {
constexpr int OPTION_COUNT = 3;
constexpr int INTRO_MAX_LINES = 4;
constexpr StrId optionNames[OPTION_COUNT] = {StrId::STR_BOOKORBIT_SETUP_WEB, StrId::STR_BOOKORBIT_SETUP_DEVICE,
                                             StrId::STR_BOOKORBIT_SETUP_LATER};
constexpr StrId optionDescs[OPTION_COUNT] = {
    StrId::STR_BOOKORBIT_SETUP_WEB_DESC, StrId::STR_BOOKORBIT_SETUP_DEVICE_DESC, StrId::STR_BOOKORBIT_SETUP_LATER_DESC};
constexpr UIIcon optionIcons[OPTION_COUNT] = {UIIcon::Wifi, UIIcon::Settings, UIIcon::Book};

uint8_t currentMissingFields() {
  return BookOrbitSetupPolicy::missingFields(!BOOKORBIT_STORE.getServerUrl().empty(),
                                             !BOOKORBIT_STORE.getUsername().empty(),
                                             !BOOKORBIT_STORE.getPassword().empty());
}
}  // namespace

void BookOrbitSetupGuideActivity::recordHandledIfConfigured() {
  if (!BookOrbitSetupPolicy::shouldRecordHandled(currentMissingFields(), SETTINGS.bookOrbitSetupGuideDismissed != 0)) {
    return;
  }
  SETTINGS.bookOrbitSetupGuideDismissed = 1;
  // Kept in memory even if the write fails; the next boot with complete credentials retries it.
  if (!SETTINGS.saveToFile()) LOG_ERR("BOSetup", "Could not persist the setup-guide handled marker");
}

void BookOrbitSetupGuideActivity::onEnter() {
  Activity::onEnter();

  const uint8_t missing = currentMissingFields();
  if (BookOrbitSetupPolicy::isConfigured(missing)) {
    // Finished on this device (or already configured): record it once, nothing left to guide.
    recordHandledIfConfigured();
    finish();
    return;
  }

  missingLine.clear();
  if (BookOrbitSetupPolicy::isPartial(missing)) {
    missingLine = tr(STR_BOOKORBIT_SETUP_STILL_NEEDED);
    const char* sep = "";  // the label carries its own trailing separator
    if (missing & BookOrbitSetupPolicy::MISSING_SERVER_URL) {
      missingLine += sep;
      missingLine += tr(STR_BOOKORBIT_SERVER_URL);
      sep = " / ";
    }
    if (missing & BookOrbitSetupPolicy::MISSING_USERNAME) {
      missingLine += sep;
      missingLine += tr(STR_USERNAME);
      sep = " / ";
    }
    if (missing & BookOrbitSetupPolicy::MISSING_PASSWORD) {
      missingLine += sep;
      missingLine += tr(STR_PASSWORD);
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int textWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const auto measure = [this](const std::string& s) { return renderer.getTextWidth(UI_10_FONT_ID, s.c_str()); };
  introLines = journal::wrapTitle(tr(STR_BOOKORBIT_SETUP_INTRO), textWidth, INTRO_MAX_LINES, measure);
  if (!missingLine.empty()) {
    const auto measureBold = [this](const std::string& s) {
      return renderer.getTextWidth(UI_10_FONT_ID, s.c_str(), EpdFontFamily::BOLD);
    };
    missingLine = journal::truncateToWidth(missingLine, textWidth, measureBold);
  }

  // Every line advances by the tallest font any drawn line resolves to (see GfxRenderer::resolveTextFontId).
  textLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& line : introLines) {
    textLineHeight = std::max(textLineHeight, renderer.getLineHeight(UI_10_FONT_ID, line.c_str()));
  }
  if (!missingLine.empty()) {
    textLineHeight =
        std::max(textLineHeight, renderer.getLineHeight(UI_10_FONT_ID, missingLine.c_str(), EpdFontFamily::BOLD));
  }
  const int textLines = static_cast<int>(introLines.size()) + (missingLine.empty() ? 0 : 1);
  listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2 + textLines * textLineHeight +
            metrics.verticalSpacing * 2;
  listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  selectedIndex = 0;
  requestUpdate();
}

void BookOrbitSetupGuideActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    dismiss();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  switch (handleListTouch(selectedIndex, OPTION_COUNT, listTop, listHeight, true)) {
    case ListTouchResult::Activated:
      activate();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, OPTION_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, OPTION_COUNT);
    requestUpdate();
  });
}

void BookOrbitSetupGuideActivity::activate() {
  if (selectedIndex == 0) {
    // Leaves Home for the web server; its exit reboots, and Home shows this guide again only while
    // BookOrbit is still incomplete and not dismissed.
    activityManager.goToBookOrbitWebSetup();
  } else if (selectedIndex == 1) {
    startActivityForResult(std::make_unique<BookOrbitSettingsActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { onEnter(); });
  } else {
    dismiss();
  }
}

void BookOrbitSetupGuideActivity::dismiss() {
  if (SETTINGS.bookOrbitSetupGuideDismissed == 0) {
    SETTINGS.bookOrbitSetupGuideDismissed = 1;
    // Kept in memory even if the write fails, so this session is not interrupted again.
    if (!SETTINGS.saveToFile()) LOG_ERR("BOSetup", "Could not persist the setup-guide dismissal");
  }
  finish();
}

void BookOrbitSetupGuideActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKORBIT_SETUP_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  for (const auto& line : introLines) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str(), true);
    y += textLineHeight;
  }
  if (!missingLine.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, missingLine.c_str(), true, EpdFontFamily::BOLD);
  }

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, OPTION_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(optionNames[index])); },
      [](int index) { return std::string(I18N.get(optionDescs[index])); },
      [](int index) { return optionIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
