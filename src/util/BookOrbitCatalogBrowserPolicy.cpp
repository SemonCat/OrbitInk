#include "BookOrbitCatalogBrowserPolicy.h"

#include <algorithm>

#include "OpdsFilename.h"
#include "StringUtils.h"

namespace BookOrbitCatalogBrowserPolicy {

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
