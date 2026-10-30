#pragma once

#include <BookOrbitCatalogClient.h>
#include <BookOrbitOwnerId.h>

#include <cstdint>
#include <string>
#include <vector>

// Owner-scoped offline cache for the BookOrbit library browser (docs/bookorbit-home-library.md §4.2):
// the last successful root sections, and the first page of each visited book query. Read ONLY after
// a transport-level failure (no HTTP status) -- never after an HTTP or auth error, so a revoked
// account or a server-side change is never masked by stale data.
//
// Lives in the same owner directory as the download index (/.crosspoint/bookorbit/lib_<key>/cache),
// each file headed by the full owner, so another server/account never reads it. At most
// MAX_FILES files; writing a new one past that removes the oldest cache file (our own artifacts
// only). Writes go through AtomicFileReplace and skip identical bytes.
namespace BookOrbitCatalogCache {

constexpr size_t MAX_FILES = 16;

struct Meta {
  uint32_t savedAt = 0;  // Unix seconds, 0 when the clock was unknown at save time
};

class Cache {
 public:
  Cache(const BookOrbitOwnerId& owner, BookOrbitSha256Fn sha256);
  bool enabled() const { return !owner_.isZero(); }

  bool saveRoot(const std::vector<BookOrbitCatalogClient::Section>& sections, uint32_t nowSeconds);
  bool loadRoot(std::vector<BookOrbitCatalogClient::Section>& out, Meta& meta);

  // Keyed by the canonical query (filters, sort, readStatus, search text, page).
  bool saveBooks(const BookOrbitCatalogClient::BookQuery& query, const BookOrbitCatalogClient::BookPage& page,
                 uint32_t nowSeconds);
  bool loadBooks(const BookOrbitCatalogClient::BookQuery& query, BookOrbitCatalogClient::BookPage& out, Meta& meta);

  std::string directory() const { return dir_; }

 private:
  std::string pathFor(const std::string& key) const;
  std::string queryKey(const BookOrbitCatalogClient::BookQuery& query) const;
  bool writeFile(const std::string& path, const std::string& bytes);
  bool readFile(const std::string& path, std::string& bytes);
  void evictIfNeeded(const std::string& keepPath);

  BookOrbitOwnerId owner_;
  BookOrbitSha256Fn sha256_ = nullptr;
  std::string dir_;
};

// Canonical, unambiguous (length-framed) encoding of a query, hashed into its cache file name.
std::string queryKeyMaterial(const BookOrbitCatalogClient::BookQuery& query);

}  // namespace BookOrbitCatalogCache
