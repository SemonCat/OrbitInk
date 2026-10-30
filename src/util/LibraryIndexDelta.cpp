#include "LibraryIndexDelta.h"

#include <algorithm>
#include <iterator>

namespace LibraryIndexDelta {

CarryOverMatcher::CarryOverMatcher(std::vector<LibraryIndex::Entry>& oldEntries) {
  byPath.reserve(oldEntries.size());
  std::transform(oldEntries.begin(), oldEntries.end(), std::back_inserter(byPath),
                 [](LibraryIndex::Entry& entry) { return &entry; });
  std::sort(byPath.begin(), byPath.end(),
            [](const LibraryIndex::Entry* a, const LibraryIndex::Entry* b) { return a->path < b->path; });
}

bool CarryOverMatcher::takeMatch(const std::string& path, const uint32_t size, const uint32_t fatDateTime,
                                 LibraryIndex::Entry& out) {
  const auto it = std::lower_bound(byPath.begin(), byPath.end(), path,
                                   [](const LibraryIndex::Entry* e, const std::string& p) { return e->path < p; });
  if (it == byPath.end() || (*it)->path != path || (*it)->size != size || (*it)->fatDateTime != fatDateTime) {
    return false;
  }

  LibraryIndex::Entry& matched = **it;
  out.path = matched.path;  // copied on purpose -- see the header
  out.size = matched.size;
  out.fatDateTime = matched.fatDateTime;
  out.title = std::move(matched.title);
  out.author = std::move(matched.author);
  out.series = std::move(matched.series);
  out.seriesIndex = matched.seriesIndex;
  return true;
}

}  // namespace LibraryIndexDelta
