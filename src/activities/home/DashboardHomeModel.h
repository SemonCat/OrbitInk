#pragma once

#include <cstddef>

// Book-selection semantics for the Dashboard Home card (CrossPointSettings::UI_THEME::DASHBOARD).
//
// Pure decision logic -- no I/O, no HAL, no store access -- so the whole selection contract is
// host-testable in isolation (see test/dashboard_home_model/). HomeActivity supplies the answers
// this needs (how many recents there are, whether each one's backing file is still present, where
// the pinned path sits in that list) and applies the resulting order to its own RecentBook vector.
//
// The contract, in one place:
//   * A valid pinned book is slot 0 -- the default selection on EVERY Home entry. HomeActivity is
//     rebuilt from scratch by ActivityManager::goHome(), so recomputing the order there is what
//     makes browsing ephemeral: leaving Home and coming back lands on the pin again, and nothing
//     ever writes back to PinnedBookStore.
//   * The rest of the order is the recent books, most-recent-first, with the pinned path removed
//     so one book can never occupy two slots.
//   * Entries whose backing file is gone are skipped, so an invalid pin degrades to the next valid
//     recent, and a fully invalid list degrades to the empty state (a returned count of 0).
namespace dashboard {

// Slot value meaning "the pinned book, which is not in the recent-books list" -- its path and
// title come from PinnedBookStore rather than from a RecentBook. Any other slot value is an index
// into the caller's recents list.
constexpr int kPinnedSlot = -1;

// Is recents[index] usable? A plain function pointer + context rather than std::function: this is
// called at most once per candidate on the Home entry path, and std::function would heap-allocate
// its closure for no benefit (see the CLAUDE.md note on std::function bloat). The context is const
// because nothing here may mutate the caller's list.
using RecentValidFn = bool (*)(const void* ctx, int index);

struct BrowseOrderInput {
  int recentCount = 0;  // entries in the caller's recent-books list
  // SETTINGS.pinBookToHome && PINNED_BOOK.hasPinned(). Kept separate from pinnedValid so a
  // disabled pin and a pin pointing at a deleted file are distinguishable at the call site.
  bool pinEnabled = false;
  bool pinnedValid = false;    // the pinned path still resolves on the SD card
  int pinnedRecentIndex = -1;  // index of the pinned path within recents, or -1 when absent
  int maxEntries = 0;          // cap on how many books Left/Right may browse through
};

// Writes the browse order into `out` and returns how many slots were written (0 => empty state).
// Never writes more than min(maxEntries, outCapacity) slots.
inline int buildBrowseOrder(const BrowseOrderInput& in, const RecentValidFn recentValid, const void* ctx, int* out,
                            const int outCapacity) {
  const int limit = in.maxEntries < outCapacity ? in.maxEntries : outCapacity;
  if (out == nullptr || limit <= 0) return 0;

  int written = 0;
  int pinnedSlotIndex = -1;  // which recents index (if any) slot 0 already consumed

  if (in.pinEnabled && in.pinnedValid) {
    if (in.pinnedRecentIndex >= 0 && in.pinnedRecentIndex < in.recentCount) {
      // The pin is also a recent book: use that entry, which carries the author and cover path
      // the pin store itself does not keep.
      if (recentValid == nullptr || recentValid(ctx, in.pinnedRecentIndex)) {
        out[written++] = in.pinnedRecentIndex;
        pinnedSlotIndex = in.pinnedRecentIndex;
      }
    } else {
      out[written++] = kPinnedSlot;
    }
  }

  for (int i = 0; i < in.recentCount && written < limit; ++i) {
    if (i == pinnedSlotIndex) continue;
    if (recentValid != nullptr && !recentValid(ctx, i)) continue;
    out[written++] = i;
  }

  return written;
}

// Wrap-around stepping for the ephemeral browse index. Returns `index` unchanged when there is
// nothing (or only one thing) to browse, so Left/Right stay silent no-ops instead of costing a
// full-screen redraw that would paint the identical card.
constexpr int nextBrowseIndex(const int index, const int count) {
  if (count <= 1) return index;
  const int next = index + 1;
  return next >= count ? 0 : next;
}

constexpr int prevBrowseIndex(const int index, const int count) {
  if (count <= 1) return index;
  return index <= 0 ? count - 1 : index - 1;
}

// Clamps a remembered browse index onto a (possibly shrunken) order. Used when the order is
// rebuilt -- e.g. a book was deleted while the reader had it open -- so a stale index can never
// index past the end.
constexpr int clampBrowseIndex(const int index, const int count) {
  if (count <= 0) return 0;
  if (index < 0) return 0;
  return index >= count ? count - 1 : index;
}

}  // namespace dashboard
