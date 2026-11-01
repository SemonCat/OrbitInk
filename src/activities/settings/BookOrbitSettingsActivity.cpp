#include "BookOrbitSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "BookOrbitAuthActivity.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitManualSyncActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/browser/BookOrbitCatalogBrowserActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 7;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME,
                                     StrId::STR_PASSWORD,
                                     StrId::STR_BOOKORBIT_SERVER_URL,
                                     StrId::STR_TEST_CONNECTION,
                                     StrId::STR_BOOKORBIT_BROWSE_CATALOG,
                                     StrId::STR_BOOKORBIT_SYNC_NOW,
                                     StrId::STR_BOOKORBIT_SETUP_WEB};
}  // namespace

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  showSaveError = false;
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() { Activity::onExit(); }

void BookOrbitSettingsActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touchSel = static_cast<int>(selectedIndex);
  const auto listTouch = handleListTouch(touchSel, MENU_ITEMS, contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectedIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) activateSelected();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                                   BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string priorUsername = BOOKORBIT_STORE.getUsername();
                               const std::string priorPassword = BOOKORBIT_STORE.getPassword();
                               BOOKORBIT_STORE.setCredentials(kb.text, priorPassword);
                               showSaveError = !BOOKORBIT_STORE.saveToFile();
                               if (showSaveError) {
                                 LOG_ERR("BOSettings", "Could not persist username change; reverting in memory");
                                 BOOKORBIT_STORE.setCredentials(priorUsername, priorPassword);
                               }
                               requestUpdate();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                BOOKORBIT_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            const std::string priorUsername = BOOKORBIT_STORE.getUsername();
            const std::string priorPassword = BOOKORBIT_STORE.getPassword();
            BOOKORBIT_STORE.setCredentials(priorUsername, kb.text);
            showSaveError = !BOOKORBIT_STORE.saveToFile();
            if (showSaveError) {
              LOG_ERR("BOSettings", "Could not persist password change; reverting in memory");
              BOOKORBIT_STORE.setCredentials(priorUsername, priorPassword);
            }
            requestUpdate();
          }
        });
  } else if (selectedIndex == 2) {
    // Server URL -- BookOrbit is self-hosted only, no default to fall back to, so prefill with
    // https:// (its own normalization defaults to that scheme).
    const std::string currentUrl = BOOKORBIT_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               const std::string priorUrl = BOOKORBIT_STORE.getServerUrl();
                               BOOKORBIT_STORE.setServerUrl(urlToSave);
                               showSaveError = !BOOKORBIT_STORE.saveToFile();
                               if (showSaveError) {
                                 LOG_ERR("BOSettings", "Could not persist server URL change; reverting in memory");
                                 BOOKORBIT_STORE.setServerUrl(priorUrl);
                               }
                               requestUpdate();
                             }
                           });
  } else if (selectedIndex == 3) {
    // Test Connection
    if (!BOOKORBIT_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(std::make_unique<BookOrbitAuthActivity>(renderer, mappedInput),
                           [](const ActivityResult&) {});
  } else if (selectedIndex == 4) {
    // Browse Catalog
    if (!BOOKORBIT_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(std::make_unique<BookOrbitCatalogBrowserActivity>(renderer, mappedInput),
                           [](const ActivityResult&) {});
  } else if (selectedIndex == 5) {
    // Sync Now -- manual trigger; BookOrbitManualSyncActivity itself reports NO_CREDENTIALS /
    // NO_OPEN_BOOK if either is missing, so no gating is needed here (unlike index 3/4, which
    // gate before launching so their subtitle's "[Set credentials first]" hint stays accurate).
    startActivityForResult(std::make_unique<BookOrbitManualSyncActivity>(renderer, mappedInput),
                           [](const ActivityResult&) {});
  } else if (selectedIndex == 6) {
    // Set up in a web browser -- needs no credentials; the web server's exit reboots the device.
    activityManager.goToBookOrbitWebSetup();
  }
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKORBIT_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        if (index == 0) {
          auto username = BOOKORBIT_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        } else if (index == 1) {
          return BOOKORBIT_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 2) {
          auto serverUrl = BOOKORBIT_STORE.getServerUrl();
          return serverUrl.empty() ? std::string(tr(STR_NOT_SET)) : serverUrl;
        } else if (index == 3 || index == 4 || index == 5) {
          return BOOKORBIT_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        } else if (index == 6) {
          return std::string();  // web setup needs no credentials
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showSaveError) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));

  renderer.displayBuffer();
}
