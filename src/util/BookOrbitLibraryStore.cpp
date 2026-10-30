#include "BookOrbitLibraryStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstring>
#include <vector>

#include "AtomicFileReplace.h"
#include "StringUtils.h"

namespace BookOrbitLibrary {

namespace {

constexpr char MODULE[] = "BOLib";
constexpr uint8_t MAGIC[4] = {'B', 'O', 'L', 'I'};
constexpr char ROOT_DIR[] = "/.crosspoint/bookorbit";
constexpr uint8_t FLAG_VALID = 0x01;

uint8_t checksum(const uint8_t* data, const size_t len) {
  uint8_t sum = 0x3C;
  for (size_t i = 0; i < len; i++) sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ data[i]);
  return sum;
}

void putU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

// Identifier field: must fit with its NUL, or the whole encode fails. Never truncated.
bool putId(uint8_t* field, const size_t cap, const std::string& value) {
  if (value.size() >= cap || value.find('\0') != std::string::npos) return false;
  memcpy(field, value.data(), value.size());
  return true;
}

// Display field: truncated on a UTF-8 boundary to fit with its NUL.
void putText(uint8_t* field, const size_t cap, const std::string& value) {
  size_t n = value.size() < cap ? value.size() : cap - 1;
  if (n < value.size()) n = static_cast<size_t>(utf8SafeTruncateBuffer(value.data(), static_cast<int>(n)));
  memcpy(field, value.data(), n);
}

bool getString(const uint8_t* field, const size_t cap, std::string& out) {
  const void* nul = memchr(field, 0, cap);
  if (nul == nullptr) return false;  // every field must be NUL-terminated within its width
  out.assign(reinterpret_cast<const char*>(field), static_cast<const uint8_t*>(nul) - field);
  return true;
}

std::string hex(const uint8_t* bytes, const size_t n) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = kHexDigits[bytes[i] >> 4];
    s[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
  }
  return s;
}

void appendFramed(std::string& out, const std::string& field) {
  const uint32_t len = static_cast<uint32_t>(field.size());
  for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
  out += field;
}

bool isLowerHex32(const std::string& s) {
  if (s.size() != 32) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void encodeHeader(uint8_t (&out)[HEADER_BYTES], const BookOrbitOwnerId& owner) {
  memcpy(out, MAGIC, 4);
  out[4] = INDEX_VERSION;
  memcpy(out + 5, owner.bytes, BookOrbitOwnerId::BYTES);
}

}  // namespace

bool encode(const Entry& e, uint8_t (&out)[RECORD_BYTES]) {
  memset(out, 0, RECORD_BYTES);
  if (e.bookId.empty() || e.path.empty() || e.path[0] != '/') return false;
  if (!e.fileHash.empty() && !isLowerHex32(e.fileHash)) return false;
  if (!putId(out + OFF_BOOK_ID, BOOK_ID_BYTES, e.bookId) || !putId(out + OFF_FILE_ID, FILE_ID_BYTES, e.fileId) ||
      !putId(out + OFF_FILE_HASH, FILE_HASH_BYTES, e.fileHash) || !putId(out + OFF_PATH, PATH_BYTES, e.path)) {
    return false;
  }
  putText(out + OFF_TITLE, TITLE_BYTES, e.title);
  putText(out + OFF_AUTHORS, AUTHORS_BYTES, e.authors);
  putU32(out + OFF_SIZE, e.sizeBytes);
  putU32(out + OFF_DOWNLOADED_AT, e.downloadedAt);
  out[OFF_FLAGS] = FLAG_VALID;
  out[OFF_CHECKSUM] = checksum(out, OFF_CHECKSUM);
  return true;
}

bool decode(const uint8_t (&in)[RECORD_BYTES], Entry& out) {
  if (checksum(in, OFF_CHECKSUM) != in[OFF_CHECKSUM] || (in[OFF_FLAGS] & FLAG_VALID) == 0) return false;
  Entry e;
  if (!getString(in + OFF_BOOK_ID, BOOK_ID_BYTES, e.bookId) || !getString(in + OFF_FILE_ID, FILE_ID_BYTES, e.fileId) ||
      !getString(in + OFF_FILE_HASH, FILE_HASH_BYTES, e.fileHash) || !getString(in + OFF_PATH, PATH_BYTES, e.path) ||
      !getString(in + OFF_TITLE, TITLE_BYTES, e.title) || !getString(in + OFF_AUTHORS, AUTHORS_BYTES, e.authors)) {
    return false;
  }
  if (e.bookId.empty() || e.path.empty() || e.path[0] != '/') return false;
  if (!e.fileHash.empty() && !isLowerHex32(e.fileHash)) return false;
  e.sizeBytes = getU32(in + OFF_SIZE);
  e.downloadedAt = getU32(in + OFF_DOWNLOADED_AT);
  out = std::move(e);
  return true;
}

std::string ownerKey(const BookOrbitOwnerId& owner, const BookOrbitSha256Fn sha256) {
  static constexpr char TAG[] = "bo-lib-dir";
  uint8_t input[sizeof(TAG) + BookOrbitOwnerId::BYTES];
  memcpy(input, TAG, sizeof(TAG));  // includes the NUL separator
  memcpy(input + sizeof(TAG), owner.bytes, BookOrbitOwnerId::BYTES);
  uint8_t digest[32];
  sha256(input, sizeof(input), digest);
  return hex(digest, 8);
}

std::string downloadPath(const BookOrbitOwnerId& owner, const std::string& bookId, const std::string& fileId,
                         const std::string& title, const BookOrbitSha256Fn sha256) {
  std::string input("bo-dl", 6);  // tag + NUL
  input.append(reinterpret_cast<const char*>(owner.bytes), BookOrbitOwnerId::BYTES);
  appendFramed(input, bookId);
  appendFramed(input, fileId);
  uint8_t digest[32];
  sha256(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest);
  std::string stem = StringUtils::sanitizeFilename(title, DOWNLOAD_TITLE_BYTES);
  if (stem.empty()) stem = "book";
  return std::string(DOWNLOAD_DIR) + "/" + stem + " [" + hex(digest, 8) + "].epub";
}

Store::Store(const BookOrbitOwnerId& owner, const BookOrbitSha256Fn sha256) : owner_(owner) {
  if (owner_.isZero()) return;
  dir_ = std::string(ROOT_DIR) + "/lib_" + ownerKey(owner_, sha256);
  indexPath_ = dir_ + "/index.bin";
}

bool Store::openForScan(HalFile& f, size_t& records) {
  records = 0;
  if (!enabled()) return false;
  if (!AtomicFileReplace::recover(MODULE, indexPath_)) return false;
  if (!Storage.exists(indexPath_.c_str())) return true;
  if (!Storage.openFileForRead(MODULE, indexPath_, f)) return false;
  uint8_t header[HEADER_BYTES];
  if (f.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return true;
  if (memcmp(header, MAGIC, 4) != 0 || header[4] != INDEX_VERSION ||
      memcmp(header + 5, owner_.bytes, BookOrbitOwnerId::BYTES) != 0) {
    return true;  // foreign or unknown format: reads as empty; never deleted here
  }
  const size_t size = f.fileSize();
  records = size > HEADER_BYTES ? (size - HEADER_BYTES) / RECORD_BYTES : 0;
  if (records > MAX_ENTRIES) records = MAX_ENTRIES;
  return true;
}

bool Store::readRecord(HalFile& f, const size_t index, Entry& out) {
  if (!f.seekSet(HEADER_BYTES + index * RECORD_BYTES)) return false;
  if (f.read(record_, RECORD_BYTES) != static_cast<int>(RECORD_BYTES)) return false;
  return decode(record_, out);
}

size_t Store::count() {
  HalFile f;
  size_t records = 0;
  if (!openForScan(f, records)) return 0;
  size_t valid = 0;
  Entry e;
  for (size_t i = 0; i < records; i++) {
    if (readRecord(f, i, e)) valid++;
  }
  return valid;
}

bool Store::find(const std::string& bookId, Entry& out) {
  HalFile f;
  size_t records = 0;
  if (bookId.empty() || !openForScan(f, records)) return false;
  Entry e;
  for (size_t i = records; i-- > 0;) {  // newest first; upsert keeps one entry per book anyway
    if (readRecord(f, i, e) && e.bookId == bookId) {
      out = std::move(e);
      return true;
    }
  }
  return false;
}

bool Store::findByPath(const std::string& path, Entry& out) {
  HalFile f;
  size_t records = 0;
  if (path.empty() || !openForScan(f, records)) return false;
  Entry e;
  for (size_t i = records; i-- > 0;) {
    if (readRecord(f, i, e) && e.path == path) {
      out = std::move(e);
      return true;
    }
  }
  return false;
}

void Store::markDownloaded(const std::string* bookIds, const size_t count, uint8_t* flags) {
  if (flags == nullptr) return;
  for (size_t i = 0; i < count; i++) flags[i] = 0;
  if (bookIds == nullptr || count == 0) return;
  std::vector<std::string> paths(count);
  {
    HalFile f;
    size_t records = 0;
    if (!openForScan(f, records)) return;
    Entry e;
    for (size_t r = 0; r < records; r++) {
      if (!readRecord(f, r, e)) continue;
      for (size_t i = 0; i < count; i++) {
        if (bookIds[i] == e.bookId) paths[i] = e.path;  // one entry per book (upsert invariant)
      }
    }
  }  // index handle closed before the existence probes
  for (size_t i = 0; i < count; i++) {
    flags[i] = !paths[i].empty() && Storage.exists(paths[i].c_str()) ? 1 : 0;
  }
}

size_t Store::page(const size_t skip, Entry* out, const size_t max) {
  HalFile f;
  size_t records = 0;
  if (out == nullptr || max == 0 || !openForScan(f, records)) return 0;
  size_t seen = 0;
  size_t filled = 0;
  Entry e;
  for (size_t i = records; i-- > 0 && filled < max;) {
    if (!readRecord(f, i, e)) continue;
    if (seen++ < skip) continue;
    out[filled++] = std::move(e);
  }
  return filled;
}

bool Store::upsert(const Entry& entry) {
  if (!enabled() || !encode(entry, pending_)) {
    LOG_ERR(MODULE, "Refusing to index an entry whose identifiers do not fit");
    return false;
  }
  Storage.ensureDirectoryExists(ROOT_DIR);
  Storage.ensureDirectoryExists(dir_.c_str());
  const std::string tmp = AtomicFileReplace::tmpPath(indexPath_);
  size_t written = 0;
  {
    HalFile in;
    size_t records = 0;
    if (!openForScan(in, records)) return false;

    // Count surviving records first so capacity eviction drops exactly the oldest ones.
    size_t keep = 0;
    Entry e;
    for (size_t i = 0; i < records; i++) {
      if (readRecord(in, i, e) && e.bookId != entry.bookId) keep++;
    }
    size_t dropOldest = keep >= MAX_ENTRIES ? keep - (MAX_ENTRIES - 1) : 0;

    Storage.remove(tmp.c_str());
    HalFile out;
    if (!Storage.openFileForWrite(MODULE, tmp, out)) return false;
    uint8_t header[HEADER_BYTES];
    encodeHeader(header, owner_);
    bool ok = out.write(header, sizeof(header)) == sizeof(header);
    written += sizeof(header);
    for (size_t i = 0; ok && i < records; i++) {
      if (!readRecord(in, i, e) || e.bookId == entry.bookId) continue;  // corrupt or replaced: dropped
      if (dropOldest > 0) {
        dropOldest--;
        continue;
      }
      ok = out.write(record_, RECORD_BYTES) == RECORD_BYTES;  // record_ holds this record's raw bytes
      written += RECORD_BYTES;
    }
    ok = ok && out.write(pending_, RECORD_BYTES) == RECORD_BYTES;
    written += RECORD_BYTES;
    out.flush();
    if (!out.close() || !ok) {
      LOG_ERR(MODULE, "Index rewrite failed");
      Storage.remove(tmp.c_str());
      return false;
    }
  }  // `in` closes here, before any rename touches the live index
  if (!AtomicFileReplace::verifyTmp(MODULE, indexPath_, written)) return false;
  return AtomicFileReplace::promote(MODULE, indexPath_);
}

}  // namespace BookOrbitLibrary
