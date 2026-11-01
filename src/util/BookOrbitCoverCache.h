#pragma once

#include <BookOrbitCatalogClient.h>
#include <BookOrbitOwnerId.h>

#include <string>

// Optional media only. One 96 KiB scratch download, one decoder, and at most 48 small
// 1-bit BMPs per owner. No full-library bitmaps or server-provided URLs enter this cache.
class BookOrbitCoverCache {
 public:
  explicit BookOrbitCoverCache(const BookOrbitOwnerId& owner);
  std::string pathFor(const std::string& bookId) const;
  bool available(const std::string& bookId) const;
  bool fetch(BookOrbitCatalogClient::Session& session, const std::string& bookId,
             const BookOrbitCatalogClient::AbortCallback& abort, const std::vector<std::string>& visibleIds);

 private:
  std::string directory;
  bool makeRoom(const std::vector<std::string>& visibleIds);
};
