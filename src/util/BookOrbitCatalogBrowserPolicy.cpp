#include "BookOrbitCatalogBrowserPolicy.h"

#include <algorithm>

#include "OpdsFilename.h"
#include "StringUtils.h"

namespace BookOrbitCatalogBrowserPolicy {

ReadState readState(const std::string& status) {
  if (status.empty() || status == "unread") return ReadState::Unread;
  if (status == "want_to_read") return ReadState::WantToRead;
  if (status == "reading" || status == "rereading") return ReadState::Reading;
  if (status == "on_hold") return ReadState::OnHold;
  if (status == "read" || status == "skimmed") return ReadState::Finished;
  if (status == "abandoned") return ReadState::Abandoned;
  return ReadState::Other;
}

bool matchesReadFilter(const std::string& status, const std::string& filter) {
  if (filter.empty()) return true;
  const auto value = readState(status);
  if (filter == "unread")
    return value == ReadState::Unread || value == ReadState::WantToRead || value == ReadState::Other;
  if (filter == "reading") return value == ReadState::Reading || value == ReadState::OnHold;
  if (filter == "finished") return value == ReadState::Finished || value == ReadState::Abandoned;
  return false;
}

ListMove moveInList(int page, int index, int count, bool hasNext, int delta) {
  if (index + delta < 0) return page > 0 ? ListMove{page - 1, -1, true} : ListMove{page, 0, false};
  if (index + delta >= count && hasNext) return {page + 1, 0, true};
  return {page, std::max(0, std::min(index + delta, count - 1)), false};
}

std::vector<std::string> wrapTitle(std::string text, int width, int maxLines,
                                  const std::function<int(const std::string&)>& measure) {
  std::vector<std::string> lines;
  while (!text.empty() && static_cast<int>(lines.size()) < maxLines && width > 0) {
    size_t fit = 0;
    while (fit < text.size()) {
      size_t next = fit + 1;
      while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0) == 0x80) ++next;
      if (measure(text.substr(0, next)) > width) break;
      fit = next;
    }
    if (fit == text.size()) { lines.push_back(text); break; }
    const bool last = static_cast<int>(lines.size()) + 1 == maxLines;
    if (last) {
      while (fit > 0 && measure(text.substr(0, fit) + "...") > width) {
        --fit;
        while (fit > 0 && (static_cast<unsigned char>(text[fit]) & 0xc0) == 0x80) --fit;
      }
      if (measure(text.substr(0, fit) + "...") <= width) lines.push_back(text.substr(0, fit) + "...");
      break;
    }
    const size_t space = text.rfind(' ', fit);
    if (space != std::string::npos && space > 0) fit = space;
    if (fit == 0) break;  // No complete glyph fits the available width.
    lines.push_back(text.substr(0, fit));
    text.erase(0, fit);
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
  }
  return lines;
}

namespace {

constexpr size_t MAX_FILENAME_STEM_BYTES = 100;
constexpr size_t MAX_READABLE_FILE_ID_BYTES = 32;

bool isSafeReadableFileId(const std::string& fileId) {
  if (fileId.empty() || fileId.size() > MAX_READABLE_FILE_ID_BYTES) return false;
  return std::all_of(fileId.begin(), fileId.end(), [](const unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

std::string stableFileToken(const std::string& fileId) {
  if (isSafeReadableFileId(fileId)) return fileId;

  // Normal BookOrbit ids are short ASCII values and remain visible. Hashing the full bounded id
  // only handles long/unsafe server values without allowing separators or truncation collisions.
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : fileId) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  constexpr char HEX[] = "0123456789abcdef";
  std::string token(16, '0');
  for (size_t i = 0; i < token.size(); ++i) {
    token[token.size() - i - 1] = HEX[hash & 0x0f];
    hash >>= 4;
  }
  return token;
}

}  // namespace

std::string downloadPath(const std::string& authors, const std::string& title, const std::string& fileId,
                         const size_t fileChoiceCount) {
  std::string filename = opdsBookFilename(authors, title, OpdsFilenameFormat::AuthorTitle);
  if (fileChoiceCount <= 1) return "/" + filename;

  filename.resize(filename.size() - std::string(".epub").size());
  const std::string suffix = " [" + stableFileToken(fileId) + "]";
  const std::string stem = StringUtils::sanitizeFilename(filename, MAX_FILENAME_STEM_BYTES - suffix.size());
  return "/" + stem + suffix + ".epub";
}

}  // namespace BookOrbitCatalogBrowserPolicy
