#include "BookOrbitManualSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BookOrbitManualSyncActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
  performSync();
}

void BookOrbitManualSyncActivity::performSync() {
  result = BookOrbitAutoSync::syncNow(APP_STATE.openEpubPath, renderer, errorDetail);
  {
    RenderLock lock(*this);
    state = DONE;
  }
  requestUpdate();
}

void BookOrbitManualSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKORBIT_SYNC_NOW));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BOOKORBIT_SYNCING));
  } else {
    using Result = BookOrbitAutoSync::SyncNowResult;
    switch (result) {
      case Result::NO_CREDENTIALS:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SET_CREDENTIALS_FIRST));
        break;
      case Result::NO_OPEN_BOOK:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
        break;
      case Result::NO_WIFI:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_WIFI_CONN_FAILED));
        break;
      case Result::NETWORK_ERROR:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorDetail.c_str());
        break;
      case Result::LOCAL_LOAD_FAILED:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
        break;
      case Result::ALREADY_SYNCED:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_ALREADY_SYNCED), true, EpdFontFamily::BOLD);
        break;
      case Result::PULLED:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_APPLIED), true, EpdFontFamily::BOLD);
        break;
      case Result::PUSHED:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);
        break;
      case Result::CHRONOLOGY_UNAVAILABLE:
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorDetail.c_str());
        break;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitManualSyncActivity::loop() {
  if (state == DONE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
