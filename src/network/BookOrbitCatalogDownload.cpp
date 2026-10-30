// Defines bookOrbitDefaultFileFetch() (declared in lib/KOReaderSync/BookOrbitCatalogClient.h),
// the production FileFetcher BookOrbitCatalogClient::Session's default constructor wires up.
//
// Lives under src/network, not next to BookOrbitCatalogClient.cpp in lib/KOReaderSync, because it
// is the one piece of that client that needs HttpDownloader.h: PlatformIO's library dependency
// scanner does not give a lib/ translation unit visibility into src/network's headers (every
// existing HttpDownloader.h consumer -- OtaUpdater.cpp, FontDownloadActivity.cpp,
// OpdsBookBrowserActivity.cpp -- is itself under src/). Splitting this one function out keeps
// BookOrbitCatalogClient.cpp's own dependency set exactly what it was (KOSyncTransport,
// BookOrbitCredentialStore) and keeps it host-testable without pulling ESP-IDF/wolfSSL headers
// into that library at all.
#include <BookOrbitCatalogClient.h>
#include <BookOrbitCredentialStore.h>

#include "HttpDownloader.h"

// x-auth-* headers only; Authorization: Basic is added by downloadToFile itself from
// username/password, matching every other BookOrbit request's auth shape (see
// KOSyncRequestHelpers.h's applyAuthHeaders, which this mirrors without needing a KOSyncTransport
// -- the file body itself is streamed straight to SD by HttpDownloader, not accumulated through
// KOSyncTransport's bounded-string sink the JSON methods use).
BookOrbitCatalogClient::FetchOutcome bookOrbitDefaultFileFetch(const std::string& url, const std::string& tmpPath,
                                                                const BookOrbitCatalogClient::ProgressCallback& progress,
                                                                bool* cancelFlag) {
  const HttpDownloader::HeaderList headers = {
      {"x-auth-user", BOOKORBIT_STORE.getUsername()},
      {"x-auth-key", BOOKORBIT_STORE.getMd5Password()},
  };
  const HttpDownloader::DownloadError result = HttpDownloader::downloadToFile(
      url, tmpPath, progress, cancelFlag, BOOKORBIT_STORE.getUsername(), BOOKORBIT_STORE.getPassword(), headers);

  BookOrbitCatalogClient::FetchOutcome outcome;
  outcome.httpCode = HttpDownloader::lastHttpStatus;
  outcome.aborted = (result == HttpDownloader::ABORTED);
  outcome.localFileError = (result == HttpDownloader::FILE_ERROR);
  return outcome;
}
