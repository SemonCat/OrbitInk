#pragma once
#include <string>

/**
 * Calculate KOReader document ID (partial MD5 hash).
 *
 * KOReader identifies documents using a partial MD5 hash of the file content.
 * The algorithm reads 1024 bytes at specific offsets and computes the MD5 hash
 * of the concatenated data.
 *
 * Offsets are calculated as: 1024 << (2*i) for i = -1 to 10
 * Producing: 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304,
 *            16777216, 67108864, 268435456, 1073741824 bytes
 *
 * If an offset is beyond the file size, it is skipped.
 */
class KOReaderDocumentId {
 public:
  /**
   * Document identity for PROGRESS sync, honouring the user's configured match method.
   *
   * Hoisted here because both progress callers (KOReaderAutoSync and KOReaderSyncActivity) made
   * the same choice inline, and because keeping it next to calculateForStats() below is what
   * makes the difference between the two visible: progress follows the setting, stats never do.
   */
  static std::string calculateForProgress(const std::string& filePath, bool useFilenameMatching);

  /**
   * Document identity for BookOrbit reading stats. Always the binary partial MD5, whatever the
   * user's progress match method is.
   *
   * This is not a preference. BookOrbit resolves a page-stats hash through
   * resolveBookFilesByHashes, which matches an intrinsic bookFiles.fileHash (the KOReader binary
   * partial MD5), a hash-history entry, or a manual link that ALREADY EXISTS. Its page-stats
   * endpoint reports an unmatched hash back to the caller but never records it -- only
   * match-check does that, via upsertUnmatchedBooks -- and this firmware does not implement the
   * plugin's match-check/unmatched flow. So a filename MD5 has no route to ever being resolved:
   * it cannot match intrinsically, and nothing puts it anywhere a user could link it from.
   * Uploading one would mean every event coming back unmatched, forever, until it aged out.
   *
   * Progress sync is unaffected and still honours the configured method: it has a real filename
   * matching mode on the server, which is exactly what stats lack.
   */
  static std::string calculateForStats(const std::string& filePath);

  /**
   * Calculate the KOReader document hash for a file (binary/content-based).
   *
   * @param filePath Path to the file (typically an EPUB)
   * @return 32-character lowercase hex string, or empty string on failure
   */
  static std::string calculate(const std::string& filePath);

  /**
   * Calculate document hash from filename only (filename-based sync mode).
   * This is simpler and works when files have the same name across devices.
   *
   * @param filePath Path to the file (only the filename portion is used)
   * @return 32-character lowercase hex MD5 of the filename
   */
  static std::string calculateFromFilename(const std::string& filePath);

 private:
  // Size of each chunk to read at each offset
  static constexpr size_t CHUNK_SIZE = 1024;

  // Number of offsets to try (i = -1 to 10, so 12 offsets)
  static constexpr int OFFSET_COUNT = 12;

  // Calculate offset for index i: 1024 << (2*i)
  static size_t getOffset(int i);
};
