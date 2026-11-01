#include "BookOrbitCoverCache.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <cstring>
#include <algorithm>

#include "AtomicFileReplace.h"
#include "BookOrbitLibraryStore.h"

namespace {
constexpr char MODULE[] = "BOCover";
constexpr int WIDTH = 112;
constexpr int HEIGHT = 166;
constexpr size_t MAX_FILES = 48;

bool numericId(const std::string& id) {
  return !id.empty() && id.size() <= 20 && id.find_first_not_of("0123456789") == std::string::npos;
}

bool validBitmap(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead(MODULE, path, file) || file.size() > 3000) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || !bitmap.is1Bit() ||
      bitmap.getWidth() > WIDTH || bitmap.getHeight() > HEIGHT) return false;
  // Require all rows, not just a plausible header after a torn SD write.
  uint8_t pixels[WIDTH] = {}, row[16] = {};
  for (int y = 0; y < bitmap.getHeight(); ++y)
    if (bitmap.readNextRow(pixels, row) != BmpReaderError::Ok) return false;
  return true;
}

// Inspect dimensions before invoking an existing decoder. JPEG MCU scratch <= 8 KiB;
// PNG scanlines <= 2 KiB each. Progressive JPEG is deliberately declined.
bool supportedImage(HalFile& f, bool& png) {
  uint8_t head[33] = {};
  if (f.read(head, sizeof(head)) != sizeof(head)) return false;
  png = memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0;
  uint32_t width = 0, height = 0;
  if (png) {
    if (memcmp(head + 12, "IHDR", 4) != 0 || head[24] != 8 || head[28] != 0) return false;
    for (int i = 0; i < 4; ++i) {
      width = width * 256 + head[16 + i];
      height = height * 256 + head[20 + i];
    }
  } else {
    if (head[0] != 0xff || head[1] != 0xd8 || !f.seek(2)) return false;
    while (f.position() + 4 <= f.size()) {
      if (f.read() != 0xff) return false;
      int marker = f.read();
      while (marker == 0xff) marker = f.read();
      if (marker < 0 || marker == 0xda || marker == 0xd9) return false;
      const int hi = f.read(), lo = f.read();
      if (hi < 0 || lo < 0) return false;
      const int length = hi * 256 + lo;
      if (length < 2 || f.position() + length - 2 > f.size()) return false;
      if (marker == 0xc0) {
        uint8_t frame[6];
        if (length < 8 || f.read(frame, sizeof(frame)) != sizeof(frame) || frame[0] != 8) return false;
        height = frame[1] * 256 + frame[2];
        width = frame[3] * 256 + frame[4];
        break;
      }
      if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) return false;
      if (!f.seek(f.position() + length - 2)) return false;
    }
  }
  return width > 0 && width <= 512 && height > 0 && height <= 768 && f.seek(0);
}
}  // namespace

BookOrbitCoverCache::BookOrbitCoverCache(const BookOrbitOwnerId& owner) {
  if (owner.isZero()) return;
  directory = "/.crosspoint/bookorbit/lib_" + BookOrbitLibrary::ownerKey(owner, bookOrbitFirmwareSha256()) + "/covers";
  auto dir = Storage.open(directory.c_str());
  if (!dir || !dir.isDirectory()) return;
  // Recover before a render can read the cache. Never mutate a live temp from render().
  size_t scanned = 0;
  bool ok = true;
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (++scanned > 4 * MAX_FILES) { ok = false; break; }
    char name[40] = {};
    if (f.isDirectory() || !f.getName(name, sizeof(name))) continue;
    f.close();
    std::string n(name);
    if (n == "download.tmp") { ok = Storage.remove((directory + "/" + n).c_str()) && ok; continue; }
    if (n.size() < 9) continue;
    const auto suffix = n.substr(n.size() - 8);
    if (suffix != ".bmp.tmp" && suffix != ".bmp.bak") continue;
    if (!numericId(n.substr(0, n.size() - 8))) continue;
    n.resize(n.size() - 4);
    ok = AtomicFileReplace::recover(MODULE, directory + "/" + n) && ok;
  }
  if (!ok || dir.getError()) directory.clear();  // unresolved SD state: optional placeholders
}

std::string BookOrbitCoverCache::pathFor(const std::string& bookId) const {
  return directory.empty() || !numericId(bookId) ? "" : directory + "/" + bookId + ".bmp";
}

bool BookOrbitCoverCache::available(const std::string& bookId) const {
  const auto path = pathFor(bookId);
  return !path.empty() && validBitmap(path);
}

bool BookOrbitCoverCache::makeRoom(const std::vector<std::string>& visibleIds) {
  auto dir = Storage.open(directory.c_str());
  if (!dir || !dir.isDirectory()) return false;
  size_t count = 0;
  std::string victim;
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    char name[32] = {};
    if (f.isDirectory() || !f.getName(name, sizeof(name))) continue;
    const std::string n(name);
    if (n.size() < 5 || n.substr(n.size() - 4) != ".bmp" || !numericId(n.substr(0, n.size() - 4))) continue;
    ++count;
    const std::string path = directory + "/" + n;
    const std::string id = n.substr(0, n.size() - 4);
    if (std::find(visibleIds.begin(), visibleIds.end(), id) == visibleIds.end() && victim.empty()) victim = path;
    if (count >= MAX_FILES) break;
  }
  if (dir.getError()) return false;
  dir.close();
  return count < MAX_FILES || (!victim.empty() && Storage.remove(victim.c_str()));
}

bool BookOrbitCoverCache::fetch(BookOrbitCatalogClient::Session& session, const std::string& bookId,
                               const BookOrbitCatalogClient::AbortCallback& abort,
                               const std::vector<std::string>& visibleIds) {
  const auto path = pathFor(bookId);
  if (path.empty() || !Storage.ensureDirectoryExists(directory.c_str()) ||
      !AtomicFileReplace::recover(MODULE, path)) return false;
  if (available(bookId)) return true;
  if (!makeRoom(visibleIds)) return false;
  const std::string raw = directory + "/download.tmp";
  const auto error = session.fetchThumbnail(bookId, raw, abort);
  if (error != BookOrbitCatalogClient::OK) return false;
  HalFile input, output;
  bool png = false;
  bool ok = Storage.openFileForRead(MODULE, raw, input) && supportedImage(input, png);
  if (ok) ok = Storage.openFileForWrite(MODULE, AtomicFileReplace::tmpPath(path), output);
  if (ok) {
    ok = png ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, WIDTH, HEIGHT, false) :
               JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, WIDTH, HEIGHT, false);
  }
  if (output) ok = output.close() && ok;
  input.close();
  Storage.remove(raw.c_str());
  const std::string tmp = AtomicFileReplace::tmpPath(path);
  if (!ok || !validBitmap(tmp)) { Storage.remove(tmp.c_str()); return false; }
  return AtomicFileReplace::promote(MODULE, path);
}
