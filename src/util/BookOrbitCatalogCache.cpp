#include "BookOrbitCatalogCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "AtomicFileReplace.h"
#include "BookOrbitLibraryStore.h"

namespace BookOrbitCatalogCache {

namespace {

constexpr char MODULE[] = "BOCache";
constexpr uint8_t MAGIC[4] = {'B', 'O', 'C', 'C'};
constexpr uint8_t VERSION = 1;
constexpr uint8_t KIND_ROOT = 1;
constexpr uint8_t KIND_BOOKS = 2;
// magic(4) version(1) owner(16) kind(1) savedAt(4) hasNext(1) count(2)
constexpr size_t HEADER_BYTES = 4 + 1 + BookOrbitOwnerId::BYTES + 1 + 4 + 1 + 2;
constexpr size_t MAX_FILE_BYTES = 16 * 1024;
constexpr size_t MAX_ITEMS = static_cast<size_t>(BookOrbitCatalogClient::PAGE_SIZE) * 2;
constexpr size_t MAX_FIELD_BYTES = 255;

uint8_t checksum(const char* data, const size_t len) {
  uint8_t sum = 0x69;
  for (size_t i = 0; i < len; i++) sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ static_cast<uint8_t>(data[i]));
  return sum;
}
uint8_t checksum(const std::string& bytes) { return checksum(bytes.data(), bytes.size()); }

void putU16(std::string& out, const uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>(v >> 8));
}
void putU32(std::string& out, const uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
// Every cached string comes from BookOrbitCatalogClient, which already bounds it well below this;
// a longer one is refused so a cache file can never silently hold a different (truncated) value.
bool putStr(std::string& out, const std::string& s) {
  if (s.size() > MAX_FIELD_BYTES) return false;
  out.push_back(static_cast<char>(s.size()));
  out += s;
  return true;
}

class Reader {
 public:
  explicit Reader(const std::string& b) : b_(b) {}
  bool u8(uint8_t& v) {
    if (pos_ + 1 > b_.size()) return false;
    v = static_cast<uint8_t>(b_[pos_++]);
    return true;
  }
  bool u16(uint16_t& v) {
    uint8_t lo = 0, hi = 0;
    if (!u8(lo) || !u8(hi)) return false;
    v = static_cast<uint16_t>(lo | hi << 8);
    return true;
  }
  bool u32(uint32_t& v) {
    v = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t byte = 0;
      if (!u8(byte)) return false;
      v |= static_cast<uint32_t>(byte) << (8 * i);
    }
    return true;
  }
  bool str(std::string& s) {
    uint8_t n = 0;
    if (!u8(n) || pos_ + n > b_.size()) return false;
    s.assign(b_, pos_, n);
    pos_ += n;
    return true;
  }
  bool bytes(uint8_t* out, const size_t n) {
    if (pos_ + n > b_.size()) return false;
    memcpy(out, b_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  size_t pos() const { return pos_; }

 private:
  const std::string& b_;
  size_t pos_ = 0;
};

std::string header(const BookOrbitOwnerId& owner, const uint8_t kind, const uint32_t savedAt, const bool hasNext,
                   const uint16_t count) {
  std::string h(reinterpret_cast<const char*>(MAGIC), 4);
  h.push_back(static_cast<char>(VERSION));
  h.append(reinterpret_cast<const char*>(owner.bytes), BookOrbitOwnerId::BYTES);
  h.push_back(static_cast<char>(kind));
  putU32(h, savedAt);
  h.push_back(hasNext ? 1 : 0);
  putU16(h, count);
  return h;
}

// Validates checksum + header for `owner`/`kind` and leaves `r` positioned at the first item.
bool openPayload(const std::string& bytes, const BookOrbitOwnerId& owner, const uint8_t kind, Reader& r, Meta& meta,
                 bool& hasNext, uint16_t& count) {
  if (bytes.size() < HEADER_BYTES + 1) return false;
  const std::string body = bytes.substr(0, bytes.size() - 1);
  if (checksum(body) != static_cast<uint8_t>(bytes.back())) return false;
  uint8_t magic[4];
  uint8_t version = 0, k = 0, more = 0;
  uint8_t ownerBytes[BookOrbitOwnerId::BYTES];
  if (!r.bytes(magic, 4) || memcmp(magic, MAGIC, 4) != 0 || !r.u8(version) || version != VERSION) return false;
  if (!r.bytes(ownerBytes, sizeof(ownerBytes)) || memcmp(ownerBytes, owner.bytes, sizeof(ownerBytes)) != 0) {
    return false;  // another account's cache: never read
  }
  if (!r.u8(k) || k != kind || !r.u32(meta.savedAt) || !r.u8(more) || !r.u16(count)) return false;
  hasNext = more != 0;
  return count <= MAX_ITEMS;
}

}  // namespace

std::string queryKeyMaterial(const BookOrbitCatalogClient::BookQuery& q) {
  std::string m("bo-cache-q", 11);
  for (const std::string* f : {&q.searchText, &q.authorId, &q.seriesId, &q.libraryId, &q.collectionId, &q.smartScopeId,
                               &q.sort, &q.readStatus}) {
    putU32(m, static_cast<uint32_t>(f->size()));
    m += *f;
  }
  putU32(m, static_cast<uint32_t>(q.page));
  return m;
}

Cache::Cache(const BookOrbitOwnerId& owner, const BookOrbitSha256Fn sha256) : owner_(owner), sha256_(sha256) {
  if (owner_.isZero()) return;
  dir_ = "/.crosspoint/bookorbit/lib_" + BookOrbitLibrary::ownerKey(owner_, sha256_) + "/cache";
}

std::string Cache::pathFor(const std::string& key) const { return dir_ + "/" + key + ".bin"; }

std::string Cache::queryKey(const BookOrbitCatalogClient::BookQuery& query) const {
  const std::string material = queryKeyMaterial(query);
  uint8_t digest[32];
  sha256_(reinterpret_cast<const uint8_t*>(material.data()), material.size(), digest);
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string key(16, '0');
  for (size_t i = 0; i < 8; i++) {
    key[2 * i] = kHexDigits[digest[i] >> 4];
    key[2 * i + 1] = kHexDigits[digest[i] & 0x0F];
  }
  return key;
}

bool Cache::writeFile(const std::string& path, const std::string& bytes) {
  if (bytes.size() > MAX_FILE_BYTES) return false;
  if (!AtomicFileReplace::recover(MODULE, path)) return false;
  // Identical content (ignoring the savedAt stamp, which changes on every visit): no SD write.
  {
    std::string existing;
    // Only a file that is itself intact may stand in for the new bytes: its trailing checksum must
    // verify over everything before it (computed in place, no copy). A corrupt file -- even one
    // differing only in savedAt or the checksum byte -- is rewritten, so a healthy online fetch
    // always repairs the offline cache. The compared prefix binds magic/version/owner/kind.
    if (readFile(path, existing) && existing.size() == bytes.size() && existing.size() > HEADER_BYTES &&
        checksum(existing.data(), existing.size() - 1) == static_cast<uint8_t>(existing.back())) {
      constexpr size_t SAVED_AT_OFF = 4 + 1 + BookOrbitOwnerId::BYTES + 1;
      if (existing.compare(0, SAVED_AT_OFF, bytes, 0, SAVED_AT_OFF) == 0 &&
          existing.compare(SAVED_AT_OFF + 4, existing.size() - SAVED_AT_OFF - 5, bytes, SAVED_AT_OFF + 4,
                           bytes.size() - SAVED_AT_OFF - 5) == 0) {
        return true;
      }
    }
  }
  Storage.ensureDirectoryExists("/.crosspoint/bookorbit");
  Storage.ensureDirectoryExists(dir_.substr(0, dir_.size() - sizeof("/cache") + 1).c_str());
  Storage.ensureDirectoryExists(dir_.c_str());
  const std::string tmp = AtomicFileReplace::tmpPath(path);
  Storage.remove(tmp.c_str());
  {
    HalFile f;
    if (!Storage.openFileForWrite(MODULE, tmp, f)) return false;
    const bool ok = f.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) == bytes.size();
    f.flush();
    if (!f.close() || !ok) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (!AtomicFileReplace::verifyTmp(MODULE, path, bytes.size())) return false;
  if (!AtomicFileReplace::promote(MODULE, path)) return false;
  evictIfNeeded(path);
  return true;
}

bool Cache::readFile(const std::string& path, std::string& bytes) {
  if (!AtomicFileReplace::recover(MODULE, path) || !Storage.exists(path.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead(MODULE, path, f)) return false;
  const size_t size = f.fileSize();
  if (size == 0 || size > MAX_FILE_BYTES) return false;
  bytes.resize(size);
  return f.read(reinterpret_cast<uint8_t*>(&bytes[0]), size) == static_cast<int>(size);
}

void Cache::evictIfNeeded(const std::string& keepPath) {
  std::vector<std::string> names;
  names.reserve(MAX_FILES + 2);
  {
    auto dir = Storage.open(dir_.c_str());
    if (!dir || !dir.isDirectory()) return;
    dir.rewindDirectory();
    char name[48];
    size_t scanned = 0;
    for (auto file = dir.openNextFile(); file && scanned < 4 * MAX_FILES; file = dir.openNextFile(), scanned++) {
      if (file.isDirectory() || file.getName(name, sizeof(name)) == 0) continue;
      const size_t n = strlen(name);
      if (n == 16 + 4 && strcmp(name + 16, ".bin") == 0) names.emplace_back(name);  // our <key>.bin only
    }
  }
  while (names.size() > MAX_FILES) {
    size_t oldest = names.size();
    uint32_t oldestAt = UINT32_MAX;
    for (size_t i = 0; i < names.size(); i++) {
      const std::string path = dir_ + "/" + names[i];
      if (path == keepPath) continue;
      uint8_t h[HEADER_BYTES] = {};
      HalFile f;
      uint32_t at = 0;
      if (Storage.openFileForRead(MODULE, path, f) && f.read(h, sizeof(h)) == static_cast<int>(sizeof(h))) {
        const size_t off = 4 + 1 + BookOrbitOwnerId::BYTES + 1;
        at = static_cast<uint32_t>(h[off]) | static_cast<uint32_t>(h[off + 1]) << 8 |
             static_cast<uint32_t>(h[off + 2]) << 16 | static_cast<uint32_t>(h[off + 3]) << 24;
      }
      if (at < oldestAt) {
        oldestAt = at;
        oldest = i;
      }
    }
    if (oldest == names.size()) return;
    Storage.remove((dir_ + "/" + names[oldest]).c_str());
    names.erase(names.begin() + static_cast<long>(oldest));
  }
}

bool Cache::saveRoot(const std::vector<BookOrbitCatalogClient::Section>& sections, const uint32_t nowSeconds) {
  if (!enabled() || sections.size() > MAX_ITEMS) return false;
  std::string bytes = header(owner_, KIND_ROOT, nowSeconds, false, static_cast<uint16_t>(sections.size()));
  for (const auto& s : sections) {
    if (!putStr(bytes, s.id) || !putStr(bytes, s.title)) return false;
  }
  bytes.push_back(static_cast<char>(checksum(bytes)));
  return writeFile(pathFor("root000000000000"), bytes);
}

bool Cache::loadRoot(std::vector<BookOrbitCatalogClient::Section>& out, Meta& meta) {
  std::string bytes;
  if (!enabled() || !readFile(pathFor("root000000000000"), bytes)) return false;
  Reader r(bytes);
  bool hasNext = false;
  uint16_t count = 0;
  if (!openPayload(bytes, owner_, KIND_ROOT, r, meta, hasNext, count)) return false;
  std::vector<BookOrbitCatalogClient::Section> items;
  items.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    BookOrbitCatalogClient::Section s;
    if (!r.str(s.id) || !r.str(s.title)) return false;
    items.push_back(std::move(s));
  }
  out = std::move(items);
  return true;
}

bool Cache::saveBooks(const BookOrbitCatalogClient::BookQuery& query, const BookOrbitCatalogClient::BookPage& page,
                      const uint32_t nowSeconds) {
  if (!enabled() || page.items.size() > MAX_ITEMS) return false;
  std::string bytes = header(owner_, KIND_BOOKS, nowSeconds, page.hasNext, static_cast<uint16_t>(page.items.size()));
  for (const auto& b : page.items) {
    if (!putStr(bytes, b.id) || !putStr(bytes, b.title) || !putStr(bytes, b.authors) || !putStr(bytes, b.readStatus)) {
      return false;
    }
    const int16_t bp = b.progressPercent >= 0.0f ? static_cast<int16_t>(b.progressPercent * 100.0f + 0.5f) : -1;
    putU16(bytes, static_cast<uint16_t>(bp));
  }
  bytes.push_back(static_cast<char>(checksum(bytes)));
  const std::string key = queryKey(query);
  return writeFile(pathFor(key), bytes);
}

bool Cache::loadBooks(const BookOrbitCatalogClient::BookQuery& query, BookOrbitCatalogClient::BookPage& out,
                      Meta& meta) {
  if (!enabled()) return false;
  const std::string key = queryKey(query);
  std::string bytes;
  if (!readFile(pathFor(key), bytes)) return false;
  Reader r(bytes);
  bool hasNext = false;
  uint16_t count = 0;
  if (!openPayload(bytes, owner_, KIND_BOOKS, r, meta, hasNext, count)) return false;
  BookOrbitCatalogClient::BookPage page;
  page.hasNext = hasNext;
  page.items.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    BookOrbitCatalogClient::BookSummary b;
    uint16_t bp = 0;
    if (!r.str(b.id) || !r.str(b.title) || !r.str(b.authors) || !r.str(b.readStatus) || !r.u16(bp)) return false;
    const int16_t signedBp = static_cast<int16_t>(bp);
    b.progressPercent = signedBp >= 0 ? static_cast<float>(signedBp) / 100.0f : -1.0f;
    page.items.push_back(std::move(b));
  }
  out = std::move(page);
  return true;
}

}  // namespace BookOrbitCatalogCache
