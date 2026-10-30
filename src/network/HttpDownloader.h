#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  // Extra request headers as (name, value) pairs, e.g. for provider-specific
  // auth schemes that aren't HTTP Basic (username/password stay separate for
  // that case). Kept as a small vector, not a map: call sites pass at most a
  // couple of headers and never need to look one up by name.
  using HeaderList = std::vector<std::pair<std::string, std::string>>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  // HTTP status code of the most recently completed request on this thread
  // (0 if the request never reached a server, e.g. DNS/connect failure).
  // Single-threaded by construction: CrossPoint's network calls all run on
  // one task at a time, so a plain static needs no synchronization. Lets a
  // caller distinguish "server answered with an error" (check lastHttpStatus)
  // from "transport never completed" (status stays 0) when a fetch fails.
  static int lastHttpStatus;

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", const HeaderList& extraHeaders = {});

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", const HeaderList& extraHeaders = {});

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", const HeaderList& extraHeaders = {});

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const HeaderList& extraHeaders = {});
};
