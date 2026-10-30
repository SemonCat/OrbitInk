#pragma once

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "util/ButtonNavigator.h"

/**
 * BookOrbit-first Home for UI theme BOOKORBIT (docs/bookorbit-home-library.md §2): one
 * Continue Reading hero, up to two "also reading" rows, then 書庫 / 搜尋 / 設定 tiles. There is no
 * separate local-library entry; the library is BookOrbit's.
 *
 * No network and no EPUB parse on paint: titles/authors come from RECENT_BOOKS (the reader records
 * every open, so a just-downloaded and opened book is the hero next time), percentages from
 * RecentBookProgress (progress.bin + book.bin spine sizes, cached), and 已下載 markers from the
 * owner-scoped download index. A missing hero cover thumbnail is generated once AFTER the first
 * paint from the existing metadata cache, exactly like the legacy Home's loadRecentCovers().
 *
 * UI theme JOURNAL uses the same data, focus order and actions with the editorial layout in
 * components/themes/journal/JournalHomeLayout.h: cover-bearing hero plus two recent columns. Its
 * covers are exact-box thumbnails generated after paint, one per pass, bounded to the three shown.
 */
class BookOrbitHomeActivity final : public Activity {
 public:
  explicit BookOrbitHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 HomeMenuItem initialMenuItem = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItem) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int MAX_RECENT = 3;  // hero + 2 rows
  enum class Tile { Library, Search, Settings };
  static constexpr int TILE_COUNT = 3;

  struct Shown {
    RecentBook book;
    float percent = -1.0f;
    bool downloaded = false;  // a BookOrbit library copy for the current account
  };

  const HomeMenuItem initialMenuItem;
  ButtonNavigator buttonNavigator;
  std::vector<Shown> shown;  // [0] is the hero when non-empty
  bool bookOrbitConfigured = false;
  int focus = 0;  // 0..rows-1 = hero/also-reading, then tiles
  bool backReleasePending = true;
  bool confirmReleasePending = true;
  bool firstRenderDone = false;
  bool coverPassDone = false;
  bool journal = false;
  // Journal cover boxes from the last layout, and whether a thumbnail generation was already
  // attempted for each shown slot (never retried within one Home visit).
  struct CoverBox {
    int width = 0;
    int height = 0;
  };
  CoverBox journalCoverBox[MAX_RECENT];
  bool journalCoverTried[MAX_RECENT] = {};
  struct JournalTextCache {
    int width = -1;
    int maxLines = 0;
    int font = 0;
    int style = 0;
    std::vector<std::string> lines;
    std::string author;
  };
  JournalTextCache journalText[MAX_RECENT];

  int bookSlots() const { return static_cast<int>(shown.size()); }
  int focusCount() const { return bookSlots() == 0 ? 1 + TILE_COUNT : bookSlots() + TILE_COUNT; }
  // Focus index of a tile. With no recent book the empty hero still takes slot 0.
  int tileFocus(Tile t) const { return (bookSlots() == 0 ? 1 : bookSlots()) + static_cast<int>(t); }
  void activate();
  bool loadHeroCover();
  void openTile(Tile t);
  void drawHero(int x, int y, int w, int h, bool focused);
  void drawRow(const Shown& s, int x, int y, int w, int h, bool focused);
  void drawTile(const char* label, int x, int y, int w, int h, bool focused);
  void renderJournal();
  bool loadNextJournalCover();
  // Draws the slot's exact-box thumbnail centered in `box` when one exists, else a ruled placeholder.
  void drawJournalCover(int slot, int x, int y, int w, int h);
};
