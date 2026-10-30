#pragma once
#include <functional>
#include <string>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "util/ButtonNavigator.h"

class DashboardTheme;
struct RecentBook;
struct Rect;
struct ThemeMetrics;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  // Carousel theme only (see loop()'s isCarouselTheme branch): remembers each level's own
  // position so switching levels with Up/Down restores where you left off instead of resetting
  // to index 0 -- the carousel's last-centered book, and the menu row's last-focused icon.
  // Indices are level-local (0-based within that level), not selectorIndex's flat numbering.
  int lastCarouselIndex = 0;
  int lastMenuIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool showKoSyncSuccess = false;
  unsigned long koSyncSuccessTime = 0;
  // Doesn't affect the Transfer & Sync row's layout (it's always shown) -- only which
  // destination it opens; see onTransferAndSyncOpen.
  bool hasOpdsServers = false;
  bool pinnedBookVisible = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  // Home can be entered while Back is still held (e.g. leaving Settings with
  // Back): ignore that stale release until a fresh press is seen here.
  bool backPressSeen = false;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  // --- Dashboard theme only (CrossPointSettings::UI_THEME::DASHBOARD) -------------------------
  // Home shows ONE reading card; front Left/Right browse this list and Confirm opens whatever the
  // card currently shows. The list is rebuilt from scratch in onEnter(), and ActivityManager
  // constructs a fresh HomeActivity on every goHome() -- which is exactly what makes browsing
  // ephemeral: leaving Home and coming back lands on the pinned/default book again, and nothing
  // here ever writes to PinnedBookStore.
  static constexpr int DASHBOARD_BROWSE_MAX = 6;
  std::vector<RecentBook> dashboardBrowse;
  int dashboardBrowseIndex = 0;
  // dashboardBrowse[0] is the pinned book, so it is the default selection and gets the pin glyph.
  bool dashboardPinnedFirst = false;
  // Memo for the single small cache read the card needs (stats.bin), refreshed only when the shown
  // book changes -- so browsing costs one read per newly-shown book instead of one per frame.
  // Nothing here opens a book; see loadDashboardBookStats().
  bool dashboardHasStats = false;
  BookReadingStats dashboardStats;
  // The Dashboard replaces the whole Home surface at once (large light-gray card, inverted pill,
  // filled rows), so its first paint after entering Home is given the flashing waveform and every
  // later paint -- including Left/Right browsing -- stays on the default non-flashing one.
  bool dashboardFirstRenderDone = false;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool pinnedVisible) {
    int i = 0;
    if (pinnedVisible) {
      if (item == HomeMenuItem::PINNED) return i;
      ++i;
    }
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::TRANSFER_AND_SYNC) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool pinnedVisible) {
    int i = 0;
    if (pinnedVisible && idx == i++) return HomeMenuItem::PINNED;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (idx == i++) return HomeMenuItem::TRANSFER_AND_SYNC;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onTransferAndSyncOpen();

  // Non-null only while the Dashboard theme is active (UITheme owns the instance).
  static const DashboardTheme* dashboardTheme();
  void loadDashboardBrowseList();
  // Points recentBooks at dashboardBrowse[dashboardBrowseIndex] and refreshes the memo above.
  void syncDashboardSelection();
  void loadDashboardBookStats(const std::string& path);
  // Moves the browse position by +1/-1 and pulls the Home selection onto the card, so a
  // Left/Right followed by Confirm always opens the book the user just browsed to.
  void stepDashboardBrowse(int delta);
  void renderDashboard(const ThemeMetrics& metrics, const DashboardTheme& theme);

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks, const std::string& excludePath);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
