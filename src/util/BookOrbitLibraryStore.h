#pragma once

#include <BookOrbitOwnerId.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Owner-scoped index of BookOrbit books downloaded to this device: the "已下載" filter of the single
// BookOrbit library, and the reopen/offline lookup for a book's local copy
// (docs/bookorbit-home-library.md §4.1 and §9).
//
// Scope and privacy: every file lives under /.crosspoint/bookorbit/lib_<key>/ where <key> is a
// one-way 16-hex derivative of the current BookOrbit owner (server + account), and every file
// header carries the full owner, which reads verify. Another server/account therefore sees an empty
// index and can never alias, open or reveal this account's entries; switching credentials moves
// nothing on SD.
//
// RAM: fixed-size records streamed one at a time through the store's own record buffer; the
// file is never loaded whole. Writes rewrite through AtomicFileReplace (tmp -> verify -> promote).
//
// Identifiers are never truncated: an upsert whose bookId/fileId/fileHash/path does not fit its
// field is rejected. Only title/authors (display) are truncated, on a UTF-8 boundary.
namespace BookOrbitLibrary {

constexpr size_t BOOK_ID_BYTES = 24;  // NUL-padded; numeric server ids are <= 20 digits
constexpr size_t FILE_ID_BYTES = 24;
constexpr size_t FILE_HASH_BYTES = 33;  // 32 lowercase hex + NUL, or empty
constexpr size_t PATH_BYTES = 128;
constexpr size_t TITLE_BYTES = 96;
constexpr size_t AUTHORS_BYTES = 48;

constexpr size_t OFF_BOOK_ID = 0;
constexpr size_t OFF_FILE_ID = OFF_BOOK_ID + BOOK_ID_BYTES;
constexpr size_t OFF_FILE_HASH = OFF_FILE_ID + FILE_ID_BYTES;
constexpr size_t OFF_PATH = OFF_FILE_HASH + FILE_HASH_BYTES;
constexpr size_t OFF_TITLE = OFF_PATH + PATH_BYTES;
constexpr size_t OFF_AUTHORS = OFF_TITLE + TITLE_BYTES;
constexpr size_t OFF_SIZE = OFF_AUTHORS + AUTHORS_BYTES;  // u32 LE
constexpr size_t OFF_DOWNLOADED_AT = OFF_SIZE + 4;        // u32 LE, Unix seconds (0 = unknown)
constexpr size_t OFF_FLAGS = OFF_DOWNLOADED_AT + 4;       // u8
constexpr size_t OFF_RESERVED = OFF_FLAGS + 1;            // 5 bytes, zero
constexpr size_t OFF_CHECKSUM = OFF_RESERVED + 5;         // u8 over [0, OFF_CHECKSUM)
constexpr size_t RECORD_BYTES = OFF_CHECKSUM + 1;
static_assert(RECORD_BYTES == 368, "index record layout changed: bump INDEX_VERSION and the design doc");
static_assert(OFF_CHECKSUM == RECORD_BYTES - 1, "checksum must be the last byte");

constexpr size_t HEADER_BYTES = 4 + 1 + BookOrbitOwnerId::BYTES;  // 'BOLI', version, owner
constexpr uint8_t INDEX_VERSION = 1;
constexpr size_t MAX_ENTRIES = 512;

struct Entry {
  std::string bookId;
  std::string fileId;
  std::string fileHash;  // server's KOReader partial MD5, or empty when unknown
  std::string path;      // local EPUB path on SD
  std::string title;     // display only
  std::string authors;   // display only
  uint32_t sizeBytes = 0;
  uint32_t downloadedAt = 0;
};

// Pure codec, host-testable. encode() returns false (and writes nothing meaningful) when an
// identifier does not fit; decode() returns false on a checksum or field-contract failure.
bool encode(const Entry& e, uint8_t (&out)[RECORD_BYTES]);
bool decode(const uint8_t (&in)[RECORD_BYTES], Entry& out);

// 16 lowercase hex: first 8 bytes of SHA-256("bo-lib-dir\0" || owner.bytes). One-way: the raw
// fingerprint never appears in a path.
std::string ownerKey(const BookOrbitOwnerId& owner, BookOrbitSha256Fn sha256);

// New downloads: /BookOrbit/<title sanitized, <= 60 bytes> [<16 hex>].epub where the hex is the first
// 8 bytes of SHA-256("bo-dl\0" || owner || len-framed bookId || len-framed fileId). The title part is
// display-only; identity comes from the hash, so two accounts or two editions never share a path.
// Always <= PATH_BYTES - 1.
constexpr size_t DOWNLOAD_TITLE_BYTES = 60;
std::string downloadPath(const BookOrbitOwnerId& owner, const std::string& bookId, const std::string& fileId,
                         const std::string& title, BookOrbitSha256Fn sha256);
constexpr char DOWNLOAD_DIR[] = "/BookOrbit";
static_assert(sizeof(DOWNLOAD_DIR) - 1 + 1 + DOWNLOAD_TITLE_BYTES + 2 + 16 + 1 + 5 < PATH_BYTES,
              "the longest download path must fit the index path field");

// Owner-scoped store. A zero owner (no BookOrbit credentials) disables it: nothing is read or
// written.
class Store {
 public:
  Store(const BookOrbitOwnerId& owner, BookOrbitSha256Fn sha256);

  bool enabled() const { return !owner_.isZero(); }
  // Number of decodable entries (bounded scan).
  size_t count();
  bool find(const std::string& bookId, Entry& out);
  bool findByPath(const std::string& path, Entry& out);
  // Inserts or replaces the entry for e.bookId (one entry per book). At capacity the oldest entry
  // is dropped from the INDEX ONLY; its file stays on SD and is re-linked on the next detail open.
  bool upsert(const Entry& e);
  // One index scan for a whole list page: flags[i] = 1 when bookIds[i] has an entry whose file
  // still exists on SD. Used for list badges instead of one scan per row.
  void markDownloaded(const std::string* bookIds, size_t count, uint8_t* flags);
  // Newest-first paging: fills up to `max` entries starting `skip` entries from the newest.
  size_t page(size_t skip, Entry* out, size_t max);

  // Exposed for tests.
  const std::string& indexPath() const { return indexPath_; }

 private:
  // Recovers interrupted replacements, then reports how many records the live index holds (0 when
  // absent, unreadable or owned by another account). False when recovery is unresolved.
  bool openForScan(HalFile& f, size_t& records);
  bool readRecord(HalFile& f, size_t index, Entry& out);
  BookOrbitOwnerId owner_;
  std::string dir_;
  std::string indexPath_;
  // Record buffers live in the object (not on the stack: 368 B exceeds the 256 B local limit).
  // Construct Store on the heap (makeUniqueNoThrow) in firmware code.
  uint8_t record_[RECORD_BYTES] = {};
  uint8_t pending_[RECORD_BYTES] = {};
};

}  // namespace BookOrbitLibrary
