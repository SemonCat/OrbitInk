#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// Retain the smallest sorted prefix while enumerating an arbitrarily large directory.
// The byte budget includes each entry object and its name/path payload.
template <typename T, typename Less, typename Weight>
bool retainFileEntry(std::vector<T>& entries, std::optional<T>& cutoff, T entry, size_t& bytes, const size_t maxEntries,
                     const size_t maxBytes, const Less& less, const Weight& weight) {
  if (cutoff && !less(entry, *cutoff)) return false;
  const size_t entryBytes = weight(entry);
  if (maxEntries == 0 || entryBytes > maxBytes) {
    cutoff = std::move(entry);
    while (!entries.empty() && !less(entries.front(), *cutoff)) {
      std::pop_heap(entries.begin(), entries.end(), less);
      bytes -= weight(entries.back());
      entries.pop_back();
    }
    return false;
  }
  if (entries.size() == maxEntries && !less(entry, entries.front())) return false;
  entries.push_back(std::move(entry));
  bytes += entryBytes;
  std::push_heap(entries.begin(), entries.end(), less);
  bool complete = true;
  while (entries.size() > maxEntries || bytes > maxBytes) {
    std::pop_heap(entries.begin(), entries.end(), less);
    bytes -= weight(entries.back());
    cutoff = std::move(entries.back());
    entries.pop_back();
    complete = false;
  }
  return complete;
}
