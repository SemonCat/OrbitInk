#pragma once

#include <string>
#include <vector>

#include "activities/reader/BookReadingStats.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

class GfxRenderer;
struct RecentBook;

// Reading Dashboard theme (CrossPointSettings::UI_THEME::DASHBOARD).
//
// The Home visual language -- one rounded primary reading card carrying a "Continue Reading" pill,
// the book's identity, a rounded progress panel, rounded full-width action rows and rounded button
// hints -- is adapted from YACP's DashboardTheme
// (https://github.com/Sichroteph/YACP, commit a96da864647ddc23a28431582ad722a67ca689ba,
// src/components/themes/dashboard/DashboardTheme.cpp), MIT licensed. See NOTICE.
//
// What is deliberately NOT ported, and why:
//   * YACP's branding -- no logo watermark, no product name in the header. This is CrossFade.
//   * YACP's reading-stats schema. Its card reads start/finish dates, completion flags, day
//     streaks, time-of-day buckets and completed-book counts, none of which exist in this fork's
//     BookReadingStats/GlobalReadingStats (see their headers: rolling totals only, no date or
//     history fields). Inventing them here would mean either a new schema -- explicitly out of
//     scope for this phase -- or printing numbers the model cannot support. Only the fields this
//     fork actually persists are shown, and each is omitted rather than zero-filled when absent.
//   * YACP's progress bar and percentage. A truthful book-level percentage needs book.bin's
//     cumulative spine sizes, and every route to those opens the book (Epub::load(), or
//     BookMetadataCache::load() against the same file). Home does not open books -- so the card
//     carries no progress readout at all rather than a chapter-local number presented as book
//     progress, or a new persisted field cached to fake one. See DashboardLayout.h.
//   * YACP's cover pipeline. The card is typographic: no cover thumbnail is loaded or generated to
//     draw Home (see CrossFadeDashboard's contract in HomeActivity), so this theme never allocates
//     a cover snapshot buffer.
//   * YACP's sleep screen, its right-hand seven-row statistics column, and its reader-type icons
//     -- all of which depend on the schema above.
//
// Everything outside Home (lists, popups, keyboard, status bar, button hints) is inherited from
// RoundedRaffTheme, whose rounded vocabulary already matches this card. That is also why
// DashboardMetrics changes ONLY the Home fields: every inherited RoundedRaff routine reads
// RoundedRaffMetrics::values directly, so any non-Home field that diverged here would silently
// disagree with what UITheme::getMetrics() reports.
namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = RoundedRaffMetrics::values;
  // Restrained status band: battery (and the clock, when enabled) only -- no title, no branding.
  v.homeTopPadding = 44;
  // Home is a single card, so the "cover tile" is the card's own band. No cover is ever loaded
  // into it, which is why homeCoverHeight -- the cover-thumbnail cache key every other theme uses
  // -- is left at RoundedRaff's value and simply never read by this theme.
  //
  // The band the card is drawn into, not the card's own height -- the card sizes itself to its
  // content (see dashboard::computeCardLayout) and this is only its ceiling.
  //
  // Squeezed from both ends. Below: all four action rows must clear the button-hints band on an X3
  // in portrait (792 px tall), so 44 header + 420 band + 10 offset + 10 row inset + 4 rows + 3 gaps
  // must stay above 792 - 40 -- which leaves ~36 px spare at the built-in Latin line height and
  // ~16 px at the taller 14pt CJK fallback the zh-TW profile renders (see
  // DashboardTheme::menuRowHeight). Above: the band has to be tall enough that a CJK card with a
  // two-line title, an author and a stats panel still fits, or computeCardLayout would silently
  // give up a title line (or the panel) on that build alone -- which is what the paired
  // Latin/CJK layout tests pin. If a future translation pushes past the lower bound anyway,
  // drawButtonMenu pages the rows and shows a scrollbar rather than drawing into the hints:
  // degraded, but never clipped.
  v.homeCoverTileHeight = 420;
  // One book occupies the card at a time. Left/Right browse a wider ephemeral list that
  // HomeActivity owns; this cap is only the number of flat selector slots the card consumes.
  v.homeRecentBooksCount = 1;
  // The card IS Continue Reading, and the pinned book is the card's default selection, so neither
  // gets a menu row of its own.
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 10;
  // Dashboard never shows a Pinned menu row (HomeActivity forces pinnedBookVisible false for this
  // theme), so the fit buffer that gates that row is inert here.
  v.homePinnedRowFitBuffer = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace DashboardMetrics

// Everything the card draws, assembled by HomeActivity from stores it has already loaded. Passed
// as one struct so the card needs no store access of its own and stays a pure function of this
// snapshot -- which is what keeps "Home is cheap" checkable at the single call site.
struct DashboardCardData {
  // nullptr => empty state (no valid book to continue).
  const RecentBook* book = nullptr;
  // The card row holds the Home selection (Confirm opens exactly this book).
  bool selected = false;
  // The shown book is the pinned one, which is also the default selection on every Home entry.
  bool pinned = false;
  // stats is meaningful only when hasStats -- reading-stats tracking is opt-in
  // (CrossPointSettings::shouldTrackReadingStats) and a book with no qualifying session yet has
  // none. Never rendered as zeros; the whole panel is omitted instead.
  bool hasStats = false;
  BookReadingStats stats{};
  // Position within the ephemeral Left/Right browse list. browseCount <= 1 hides the indicator.
  int browseIndex = 0;
  int browseCount = 0;
};

class DashboardTheme final : public RoundedRaffTheme {
 public:
  // Restrained Home status band. Falls through to RoundedRaffTheme for every other screen (which
  // passes a real title); Home passes nullptr, where RoundedRaff draws nothing at all.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;

  // The primary reading card. Not an override: BaseTheme::drawRecentBookCover has no way to carry
  // reading stats or the browse position, and widening that virtual would touch all five existing
  // themes for one theme's benefit. HomeActivity reaches this through
  // UITheme::getDashboardTheme(), which is non-null only while this theme is active -- typed
  // dispatch with no RTTI (the firmware builds -fno-rtti) and no new no-op virtual on BaseTheme.
  //
  // Returns the y of the card's bottom edge. The card is sized to its content, so this is the only
  // way the caller can put the action rows directly beneath it instead of at a fixed offset that
  // would strand them below a short card.
  int drawHomeCard(GfxRenderer& renderer, Rect rect, const DashboardCardData& data) const;

  // Full-width rounded action rows (RoundedRaff's own menu sizes each pill to its label instead).
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  int getMenuBottomEdge(const GfxRenderer& renderer, int menuTop, int itemCount) const override;

  // Row height used by drawButtonMenu/getMenuBottomEdge. Text-aware so a CJK fallback line cannot
  // overflow the row (see DashboardLayout.h's header comment).
  static int menuRowHeight(const GfxRenderer& renderer);

 private:
  // BaseTheme's cover-tile entry point is still called by HomeActivity for every theme. For this
  // one the card is drawn by drawHomeCard instead, so this override exists only to make that
  // explicit and to guarantee no cover snapshot buffer is ever allocated.
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer, float progressPercent = -1.0f) const override;
};
