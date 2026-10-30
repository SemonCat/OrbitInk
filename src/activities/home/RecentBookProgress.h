#pragma once

#include <cstddef>
#include <string>

// Adapted from CrossInk (uxjulia/CrossInk 9914146, src/activities/home/RecentBookProgress.h, MIT).
//
// Home-screen reading percentage for a recent EPUB WITHOUT opening it: no Epub::load(), no
// container/OPF parse, no CSS. CrossInk has its reader write a small percent cache; this firmware
// keeps its reader unchanged, so the cache here is instead keyed by the exact progress.bin record
// it was computed from and recomputed lazily (from the book's existing book.bin spine sizes) only
// when that record changes. Percentages are 0-100; negative means unknown.
namespace RecentBookProgress {
// Saved reading percentage for an EPUB at `path`, or -1.0f when the book has never been opened
// (no cache/progress yet), is not an EPUB, or its cache cannot be read.
float loadPercent(const std::string& path);
// Returns true when progress contains a known 0-100 percentage.
bool hasPercent(float progress);
// Formats progress as a rounded percentage string such as "42%" into buf (>= 8 bytes).
// Writes an empty string when progress is unknown.
void formatPercent(float progress, char* buf, size_t bufSize);
}  // namespace RecentBookProgress
