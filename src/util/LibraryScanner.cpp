#include "LibraryScanner.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "BoundedFileList.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

bool isBookFile(const std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}

std::string joinPath(const std::string& dir, const std::string& name) { return (dir == "/" ? dir : dir + "/") + name; }

// Carries a book file's fingerprint alongside its name through the same sort sortFileList would
// apply to a plain name list -- sortFileList itself only accepts vector<string>, and for a
// directory's book files specifically (never containing a '/'-suffixed directory entry) its
// "directories first" branch never fires, so a direct naturalLess sort is equivalent.
struct BookFileInfo {
  std::string name;
  uint32_t size = 0;
  uint32_t fatDateTime = 0;
};
}  // namespace

std::vector<LibraryScanner::Entry> LibraryScanner::scanAllBooks(const std::string& root, const size_t maxBooks,
                                                                const bool collectFingerprint, ScanStats* stats) {
  std::vector<Entry> result;
  if (stats) *stats = {};
  if (maxBooks == 0 || root.size() > MAX_SCAN_PATH_BYTES) {
    return result;
  }
  // Reserve conservatively up front; grows normally past this if the card holds more,
  // capped at maxBooks total below.
  result.reserve(std::min<size_t>(maxBooks, 256));

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("LIBSCAN", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    return result;
  }

  std::vector<std::string> pendingDirs;
  pendingDirs.push_back(root);
  size_t pendingBytes = sizeof(std::string) + root.size();
  size_t resultBytes = 0;
  bool truncated = false;
  bool budgetExceeded = false;

  while (!pendingDirs.empty() && !truncated) {
    const std::string dirPath = std::move(pendingDirs.back());
    pendingDirs.pop_back();
    pendingBytes -= sizeof(std::string) + dirPath.size();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      continue;
    }
    dir.rewindDirectory();

    std::vector<std::string> subDirs;
    std::vector<BookFileInfo> bookFiles;
    std::optional<BookFileInfo> bookCutoff;
    std::optional<std::string> dirCutoff;
    size_t bookBytes = 0;
    size_t dirBytes = 0;
    const auto fileLess = [](const BookFileInfo& a, const BookFileInfo& b) {
      return FsHelpers::naturalLess(a.name, b.name);
    };
    const auto dirLess = [](const std::string& a, const std::string& b) { return FsHelpers::naturalLess(a, b); };
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
      if ((!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') ||
          strcmp(nameBuffer.get(), "System Volume Information") == 0) {
        continue;
      }
      if (file.isDirectory()) {
        const auto path = joinPath(dirPath, nameBuffer.get());
        if (path.size() > MAX_SCAN_PATH_BYTES ||
            !retainFileEntry(subDirs, dirCutoff, path, dirBytes, MAX_PENDING_DIRS - pendingDirs.size(),
                             DIRECTORY_BYTE_BUDGET - pendingBytes, dirLess,
                             [](const std::string& s) { return sizeof(s) + s.size(); })) {
          budgetExceeded = true;
        }
      } else if (isBookFile(nameBuffer.get())) {
        BookFileInfo info;
        info.name = nameBuffer.get();
        if (collectFingerprint) {
          info.size = static_cast<uint32_t>(file.fileSize());
          uint16_t fatDate = 0;
          uint16_t fatTime = 0;
          if (file.getModifyTime(fatDate, fatTime)) {
            info.fatDateTime = (static_cast<uint32_t>(fatDate) << 16) | fatTime;
          }
        }
        if (!retainFileEntry(bookFiles, bookCutoff, std::move(info), bookBytes, maxBooks - result.size(),
                             DIRECTORY_BYTE_BUDGET, fileLess,
                             [](const BookFileInfo& f) { return sizeof(f) + f.name.size(); })) {
          budgetExceeded = true;
        }
      }
      if (stats) {
        stats->directoryEntries = std::max(stats->directoryEntries, bookFiles.size() + subDirs.size());
        stats->directoryBytes = std::max(stats->directoryBytes, bookBytes + dirBytes);
      }
    }
    dir.close();

    std::sort(bookFiles.begin(), bookFiles.end(),
              [](const BookFileInfo& a, const BookFileInfo& b) { return FsHelpers::naturalLess(a.name, b.name); });
    FsHelpers::sortFileList(subDirs);

    for (const auto& info : bookFiles) {
      if (result.size() >= maxBooks) {
        truncated = true;
        break;
      }
      auto path = joinPath(dirPath, info.name);
      const size_t entryBytes = sizeof(Entry) + path.size();
      if (path.size() > MAX_SCAN_PATH_BYTES || resultBytes + entryBytes > RESULT_BYTE_BUDGET) {
        truncated = true;
        break;
      }
      resultBytes += entryBytes;
      result.push_back(Entry{std::move(path), info.size, info.fatDateTime});
    }
    if (result.size() >= maxBooks) {
      truncated = true;
      break;
    }

    // Push in reverse so the worklist (a LIFO stack) still visits subfolders in
    // ascending name order.
    for (auto it = subDirs.rbegin(); it != subDirs.rend(); ++it) {
      pendingBytes += sizeof(std::string) + it->size();
      pendingDirs.push_back(std::move(*it));
    }
    if (stats) {
      stats->pendingDirectories = std::max(stats->pendingDirectories, pendingDirs.size());
      stats->pendingBytes = std::max(stats->pendingBytes, pendingBytes);
    }
  }

  if (stats) {
    stats->resultBytes = resultBytes;
    stats->capped = truncated || budgetExceeded;
  }
  if (truncated || budgetExceeded) {
    LOG_INF("LIBSCAN", "Library scan capped by entry/path memory budget (%d books returned)", static_cast<int>(result.size()));
  }

  return result;
}
