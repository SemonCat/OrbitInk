#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra's metrics with taller list rows: rows center their text from measured line heights, and the
// zh-TW profile's shared 14pt CJK fallback needs ~34px per line (two-line rows: title + author).
namespace JournalMetrics {
constexpr ThemeMetrics make() {
  ThemeMetrics m = LyraMetrics::values;
  m.listRowHeight = 48;
  m.listWithSubtitleRowHeight = 72;
  return m;
}
constexpr ThemeMetrics values = make();
}  // namespace JournalMetrics

// Journal (文藝刊物) theme: Lyra's metrics and list/menu geometry, restyled as an editorial page --
// white surfaces, thin rules, crop-corner selection brackets instead of filled rounded highlights,
// and plain ruled button hints. Home is BookOrbitHomeActivity's Journal layout
// (JournalHomeLayout.h); every other screen picks this up through the GUI component hooks below,
// so no activity needs Journal-specific code.
class JournalTheme : public LyraTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  int getListRowStep(bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;

  // Four L-shaped corner marks around `rect`, drawn `outset` px outside it: the Journal selection.
  static void drawCropBrackets(const GfxRenderer& renderer, Rect rect, int outset, int length, int stroke);
};
