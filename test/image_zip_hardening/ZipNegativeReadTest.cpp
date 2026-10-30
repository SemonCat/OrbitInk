// Exercises the REAL lib/ZipFile/ZipFile.cpp (compiled directly) + lib/miniz/src/InflateStream.cpp
// against a synthetic, hand-built single-entry ZIP served by a host HalStorage/HalFile stub (see
// host_stubs/HalStorage.h) whose read() can be told to fail (-1) at a specific byte offset --
// modeling a genuine SD read error mid-decompression.
//
// Covers the fix in ZipFile.cpp's zipFillCallback(): a negative HalFile::read() used to be cast
// straight to size_t, reporting SIZE_MAX bytes "available" in a small fixed-size read buffer --
// an out-of-bounds read inside InflateStream. It must instead fail cleanly: readFileToMemory()
// returns nullptr, no crash.
//
// This target builds with -fsanitize=address (see CMakeLists.txt) as a general safety net, but
// that is NOT what proves the old code's OOB read: whether tinfl's internal consumption of the
// (bogus, SIZE_MAX-"available") buffer actually walks past its 1KB allocation before erroring out
// on its own depends on the uninitialized bytes past the injected failure point, which in
// practice self-terminated (as invalid deflate data) well inside the allocation in manual
// testing -- so ASan firing here is not something this test can rely on. The OOB read is a
// structural fact of the old code (a size_t that reads as SIZE_MAX from a buffer with only
// readBufSize real bytes behind it), documented in zipFillCallback's own comment; what THIS test
// verifies, reliably and platform-independently, is the task's literal, directly observable
// requirement -- "ZIP read == -1 clean failure": readFileToMemory() must return nullptr, not
// garbage data or a crash.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "ZipFile.h"

namespace {

void appendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// A raw deflate "stored block" (BFINAL=1, BTYPE=00): 1 header byte, a 2-byte LEN, a 2-byte ~LEN,
// then the literal payload bytes verbatim. This is a valid deflate stream any RFC1951 decoder
// (including the vendored tinfl backing InflateStream) accepts, without needing a real compressor
// in this test.
std::vector<uint8_t> deflateStoredBlock(const std::string& payload) {
  std::vector<uint8_t> out;
  out.push_back(0x01);  // BFINAL=1, BTYPE=00, rest of the byte is padding (all zero bits)
  const auto len = static_cast<uint16_t>(payload.size());
  appendU16LE(out, len);
  appendU16LE(out, static_cast<uint16_t>(~len));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Builds a minimal, single-entry, DEFLATE-method ZIP (local header + data, central directory
// entry, EOCD) containing one file named "a" whose content is `payload`. Returns the full file
// bytes plus the byte offset within them where the compressed data begins (so a test can compute
// a failAtOffset that lands inside the compressed stream).
struct SyntheticZip {
  std::vector<uint8_t> bytes;
  size_t compressedDataOffset;
};

SyntheticZip buildSingleEntryDeflateZip(const std::string& payload) {
  const std::vector<uint8_t> compressed = deflateStoredBlock(payload);
  const uint32_t compressedSize = static_cast<uint32_t>(compressed.size());
  const uint32_t uncompressedSize = static_cast<uint32_t>(payload.size());
  constexpr char kName = 'a';

  std::vector<uint8_t> bytes;

  // --- Local file header (30 bytes) + 1-byte filename + compressed data ---
  const size_t localHeaderStart = bytes.size();
  appendU32LE(bytes, 0x04034b50);  // local file header signature
  appendU16LE(bytes, 20);          // version needed
  appendU16LE(bytes, 0);           // flags
  appendU16LE(bytes, 8);           // method = DEFLATED
  appendU16LE(bytes, 0);           // mod time
  appendU16LE(bytes, 0);           // mod date
  appendU32LE(bytes, 0);           // crc32 (unchecked by ZipFile::readFileToMemory)
  appendU32LE(bytes, compressedSize);
  appendU32LE(bytes, uncompressedSize);
  appendU16LE(bytes, 1);  // filename length
  appendU16LE(bytes, 0);  // extra length
  bytes.push_back(static_cast<uint8_t>(kName));
  const size_t compressedDataOffset = bytes.size();
  bytes.insert(bytes.end(), compressed.begin(), compressed.end());

  // --- Central directory entry (46 bytes) + 1-byte filename ---
  const size_t centralDirOffset = bytes.size();
  appendU32LE(bytes, 0x02014b50);  // central file header signature
  appendU16LE(bytes, 0);           // version made by
  appendU16LE(bytes, 20);          // version needed
  appendU16LE(bytes, 0);           // flags
  appendU16LE(bytes, 8);           // method = DEFLATED
  appendU16LE(bytes, 0);           // mod time
  appendU16LE(bytes, 0);           // mod date
  appendU32LE(bytes, 0);           // crc32
  appendU32LE(bytes, compressedSize);
  appendU32LE(bytes, uncompressedSize);
  appendU16LE(bytes, 1);  // filename length
  appendU16LE(bytes, 0);  // extra length
  appendU16LE(bytes, 0);  // comment length
  appendU16LE(bytes, 0);  // disk number start
  appendU16LE(bytes, 0);  // internal attributes
  appendU32LE(bytes, 0);  // external attributes
  appendU32LE(bytes, static_cast<uint32_t>(localHeaderStart));
  bytes.push_back(static_cast<uint8_t>(kName));
  const uint32_t centralDirSize = static_cast<uint32_t>(bytes.size() - centralDirOffset);

  // --- End of central directory record (22 bytes) ---
  appendU32LE(bytes, 0x06054b50);  // EOCD signature
  appendU16LE(bytes, 0);           // disk number
  appendU16LE(bytes, 0);           // disk with central dir
  appendU16LE(bytes, 1);           // entries on this disk
  appendU16LE(bytes, 1);           // total entries
  appendU32LE(bytes, centralDirSize);
  appendU32LE(bytes, static_cast<uint32_t>(centralDirOffset));
  appendU16LE(bytes, 0);  // comment length

  return {std::move(bytes), compressedDataOffset};
}

class ZipNegativeReadTest : public ::testing::Test {
 protected:
  void SetUp() override { HalStorage::resetTestFiles(); }
};

}  // namespace

TEST_F(ZipNegativeReadTest, SyntheticZipDecodesCorrectlyWithNoInjectedFailure) {
  // Sanity check on the synthetic ZIP builder itself, before relying on it for the failure test
  // below: a real, uninterrupted read through ZipFile must recover the exact original payload.
  const std::string payload = "HELLO CROSSPOINT ZIP TEST PAYLOAD";
  const SyntheticZip zip = buildSingleEntryDeflateZip(payload);
  HalStorage::registerTestFile("/book.epub", zip.bytes);

  const std::string path = "/book.epub";
  ZipFile zf(path);
  size_t outSize = 0;
  uint8_t* data = zf.readFileToMemory("a", &outSize, /*trailingNullByte=*/true);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(outSize, payload.size());
  EXPECT_EQ(std::string(reinterpret_cast<char*>(data)), payload);
  free(data);
}

TEST_F(ZipNegativeReadTest, NegativeReadDuringInflateFailsCleanlyInsteadOfReadingOutOfBounds) {
  // A payload longer than ZipFile's internal 1KB read-buffer chunk (see readFileToMemory's
  // fileReadBuffer) so the failure lands mid-stream, inside a real fill-callback cycle, not on
  // the very first read.
  const std::string payload(2000, 'x');
  const SyntheticZip zip = buildSingleEntryDeflateZip(payload);
  // Fail partway through the compressed data -- comfortably past the local header/filename this
  // ZipFile.cpp code reads through its OWN direct file.read() calls (not the fill callback), so
  // the injected failure is guaranteed to be observed by zipFillCallback() specifically.
  const size_t failAt = zip.compressedDataOffset + 3;
  HalStorage::registerTestFile("/book.epub", zip.bytes, failAt);

  const std::string path = "/book.epub";
  ZipFile zf(path);
  size_t outSize = 0;
  uint8_t* data = zf.readFileToMemory("a", &outSize);

  EXPECT_EQ(data, nullptr) << "a read() < 0 mid-inflate must fail the whole decode cleanly";
  if (data) free(data);  // would only run if the assertion above already failed
}
