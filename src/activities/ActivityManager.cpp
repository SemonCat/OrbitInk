#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/BookOrbitCatalogBrowserActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "home/BookOrbitHomeActivity.h"
#include "home/CoverGridBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/LibraryListActivity.h"
#include "home/RecentBooksActivity.h"
#include "home/TransferAndSyncActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/LibraryIndexRebuildActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/BookOrbitAutoSync.h"
#include "util/FullScreenMessageActivity.h"
#include "util/KOReaderAutoSync.h"
#include "util/ProgressSyncNetwork.h"
#include "util/ProgressSyncDeferred.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock(true, true);
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;
      const bool runCloseSync = pendingAction == PendingAction::Replace && syncOnReaderExit;
      syncOnReaderExit = false;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      // EpubReaderActivity::onExit() above has released section/EPUB memory and
      // file handles. Sync before the destination enters so the one-shot notice
      // reaches its first render without competing with reader RAM.
      if (runCloseSync) {
        // TLS setup is intentionally performed only after reader teardown to
        // preserve X3 heap. It is synchronous, so paint an explicit progress
        // overlay before entering the bounded network path instead of leaving
        // a seemingly-frozen reader frame. Failures remain silent: the
        // destination activity simply replaces this overlay.
        GUI.drawPopup(renderer, tr(STR_SYNC_PROGRESS));
        ProgressSyncNetwork::Batch syncBatch(APP_STATE.openEpubPath, &renderer);
        KOReaderAutoSync::syncOnClose(renderer);
        BookOrbitAutoSync::syncOnClose(renderer);
      }
      if (!pendingReaderSyncPath.empty()) {
        const std::string path = std::move(pendingReaderSyncPath);
        pendingReaderSyncPath.clear();
        ProgressSyncDeferred::applyForBook(path, renderer);
        ProgressSyncNetwork::Batch syncBatch(path, &renderer);
        KOReaderAutoSync::syncOnOpen(path, renderer);
        BookOrbitAutoSync::syncOnOpen(path, renderer);
      }
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  invalidateRender();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToBookOrbitWebSetup() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, /*bookOrbitSetup=*/true));
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  // A short Back press can be configured to leave the reader directly for the
  // file browser instead of Home. That is still a book-close transition and
  // must run the same KOSync push after the reader has released its memory.
  syncOnReaderExit = isReaderActivity();
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToCoverGridBrowser() {
  replaceActivity(
      std::make_unique<CoverGridBrowserActivity>(renderer, mappedInput, CoverGridBrowserActivity::Source::Library));
}

void ActivityManager::goToLibraryList() {
  replaceActivity(std::make_unique<LibraryListActivity>(renderer, mappedInput));
}

void ActivityManager::goToLibraryIndexRebuild(std::function<void()> onDone) {
  replaceActivity(std::make_unique<LibraryIndexRebuildActivity>(renderer, mappedInput, std::move(onDone)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToCoverGridRecentBooks() {
  replaceActivity(
      std::make_unique<CoverGridBrowserActivity>(renderer, mappedInput, CoverGridBrowserActivity::Source::RecentBooks));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToTransferAndSync() {
  replaceActivity(std::make_unique<TransferAndSyncActivity>(renderer, mappedInput));
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh, const bool checkRemoteProgress,
                                 const bool skipInitialRender) {
  // End-of-book suggestions can replace one reader directly with the next.
  // Treat that as closing the current book so its final position is pushed.
  const bool replacingReader = isReaderActivity();
  syncOnReaderExit = replacingReader;
  // A timer/button wake may have left raw remote progress for this book. Apply it before any fresh
  // comparison or ReaderActivity construction; this path always has initialized fonts/layout.
  bool appliedDeferred = false;
  if (checkRemoteProgress && replacingReader) {
    // loop() destroys and syncs the old reader before consuming this new-book transaction.
    pendingReaderSyncPath = path;
  } else {
    pendingReaderSyncPath.clear();
    appliedDeferred = ProgressSyncDeferred::applyForBook(path, renderer);
  }
  if (checkRemoteProgress && !replacingReader) {
    if (ProgressSyncNetwork::available()) GUI.drawPopup(renderer, tr(STR_SYNC_PROGRESS));
    ProgressSyncNetwork::Batch syncBatch(path, &renderer);
    KOReaderAutoSync::syncOnOpen(path, renderer);
    BookOrbitAutoSync::syncOnOpen(path, renderer);
  }

  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh,
                                                   skipInitialRender && !appliedDeferred));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  syncOnReaderExit = isReaderActivity();
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser" || activityName == "CoverGridBrowser" || activityName == "LibraryList") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks" || activityName == "CoverGridRecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser" || activityName == "CrossPointWebServer" ||
               activityName == "TransferAndSync") {
      initialMenuItem = HomeMenuItem::TRANSFER_AND_SYNC;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  if (CrossPointSettings::isBookOrbitHomeTheme(SETTINGS.uiTheme)) {
    if (initialMenuItem == HomeMenuItem::NONE && currentActivity &&
        currentActivity->name == "BookOrbitCatalogBrowser") {
      initialMenuItem = HomeMenuItem::BOOKORBIT_LIBRARY;
    }
    replaceActivity(std::make_unique<BookOrbitHomeActivity>(renderer, mappedInput, initialMenuItem));
    return;
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}

void ActivityManager::goToBookOrbitLibrary(const bool search) {
  replaceActivity(
      std::make_unique<BookOrbitCatalogBrowserActivity>(renderer, mappedInput,
                                                        search ? BookOrbitCatalogBrowserActivity::EntryMode::HomeSearch
                                                               : BookOrbitCatalogBrowserActivity::EntryMode::Home));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  invalidateRender();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  invalidateRender();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

bool ActivityManager::canEnterReaderIdlePowerSaving() const {
  // X3-only: the immediate-idle cadence (50ms poll / ~10ms debounce re-poll) is only proven safe on
  // X3. A shared X3+X4 binary (e.g. the default/gh_release envs) must not apply it to X4.
  return gpio.deviceIsX3() && currentActivity && currentActivity->allowReaderIdlePowerSaving() &&
         !currentActivity->preventAutoSleep() && pendingAction == PendingAction::None &&
         !requestedUpdate.load(std::memory_order_relaxed);
}

bool ActivityManager::isQuickResumeBaseViewEligible() const {
  return currentActivity && currentActivity->isReaderActivity() && currentActivity->isQuickResumeEligibleBaseView();
}

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  invalidateRender();
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  invalidateRender();
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock(const bool wait, const bool trackRenderGeneration)
    : tracksRenderGeneration(trackRenderGeneration) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, wait ? portMAX_DELAY : 0) == pdTRUE;
  if (isLocked && tracksRenderGeneration) {
    renderGeneration = activityManager.renderGeneration.load(std::memory_order_relaxed);
  }
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

bool RenderLock::isStale() const {
  return tracksRenderGeneration && renderGeneration != activityManager.renderGeneration.load(std::memory_order_relaxed);
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
