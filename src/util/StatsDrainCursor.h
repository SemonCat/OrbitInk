#pragma once

#include <cstddef>
#include <cstring>

// Fair, bounded round-robin over the BookOrbit stats queue directory, without loading it.
//
// Directory order on FAT is arbitrary and stable, so "scan the first N entries" starves any queue
// that sits past a prefix of empty/stale/foreign files on every wake. Instead each drain window
// picks the CAPACITY lexicographically smallest stats ids strictly greater than a persisted cursor
// (wrapping to the smallest ids when fewer remain), examines them in order, and advances the
// cursor past every id it examined. Every id is therefore examined within ceil(N / CAPACITY)
// windows regardless of directory order, while RAM stays O(CAPACITY) and the scan is a single
// pass over names only (no file is opened while iterating).
//
// Pure: no I/O, no allocation. Ids are fixed 32-character lowercase hex strings.
template <size_t CAPACITY>
class StatsDrainSelector {
 public:
  static constexpr size_t ID_CHARS = 32;
  static_assert(CAPACITY > 0, "selector needs at least one slot");

  // `cursor` is the last examined id, or empty/nullptr to start from the smallest id.
  explicit StatsDrainSelector(const char* cursor) {
    hasCursor_ = cursor != nullptr && strlen(cursor) == ID_CHARS;
    if (hasCursor_) memcpy(cursor_, cursor, ID_CHARS + 1);
  }

  // Offers one candidate id (exactly ID_CHARS characters). Ids equal to `exclude` are ignored.
  void offer(const char* id, const char* exclude) {
    if (exclude != nullptr && strcmp(id, exclude) == 0) return;
    if (!hasCursor_ || strcmp(id, cursor_) > 0) {
      insert(after_, afterCount_, id);
    } else {
      insert(wrapped_, wrappedCount_, id);
    }
  }

  // Candidates in examination order: ids after the cursor ascending, then wrapped ids ascending,
  // at most CAPACITY in total.
  size_t count() const {
    const size_t n = afterCount_ + wrappedCount_;
    return n < CAPACITY ? n : CAPACITY;
  }
  const char* at(const size_t i) const { return i < afterCount_ ? after_[i] : wrapped_[i - afterCount_]; }

 private:
  // Keeps `list` as the `CAPACITY` smallest ids seen, ascending.
  static void insert(char (&list)[CAPACITY][ID_CHARS + 1], size_t& n, const char* id) {
    size_t pos = n;
    while (pos > 0 && strcmp(list[pos - 1], id) > 0) pos--;
    if (pos > 0 && strcmp(list[pos - 1], id) == 0) return;  // duplicate name
    if (pos >= CAPACITY) return;                            // larger than everything we keep
    const size_t last = n < CAPACITY ? n : CAPACITY - 1;
    for (size_t i = last; i > pos; i--) memcpy(list[i], list[i - 1], ID_CHARS + 1);
    memcpy(list[pos], id, ID_CHARS + 1);
    if (n < CAPACITY) n++;
  }

  char cursor_[ID_CHARS + 1] = {};
  bool hasCursor_ = false;
  char after_[CAPACITY][ID_CHARS + 1] = {};
  char wrapped_[CAPACITY][ID_CHARS + 1] = {};
  size_t afterCount_ = 0;
  size_t wrappedCount_ = 0;
};
