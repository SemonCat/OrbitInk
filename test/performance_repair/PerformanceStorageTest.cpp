#include <gtest/gtest.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <thread>

#include "BoundedFileList.h"
#include "LibraryScanner.h"
#include "LibraryIndexBuilder.h"
#include "LibraryGrouping.h"
#include "BookMetadataResolver.h"

namespace {
int metadataResolutions = 0;
int thumbnailConversions = 0;
}
BookMetadataResolver::Result BookMetadataResolver::resolve(const std::string& path, int width, int height) {
  ++metadataResolutions;
  Result result;
  result.title = path;
  result.author = "Author";
  if (width > 0 && height > 0) {
    ++thumbnailConversions;
    result.hasCover = true;
    result.coverGenerated = true;
    result.coverThumbPath = "/generated.bmp";
  }
  return result;
}

namespace {
void u16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(value);
  bytes.push_back(value >> 8);
}
void u32(std::vector<uint8_t>& bytes, uint32_t value) {
  u16(bytes, value);
  u16(bytes, value >> 16);
}
uint32_t crc32(const std::string& text) {
  uint32_t crc = UINT32_MAX;
  for (uint8_t byte : text) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

std::vector<uint8_t> zip(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> central;
  for (const auto& [name, text] : files) {
    std::vector<uint8_t> deflated;
    size_t offset = 0;
    do {
      const uint16_t count = std::min<size_t>(60000, text.size() - offset);
      deflated.push_back(offset + count == text.size() ? 1 : 0);
      u16(deflated, count);
      u16(deflated, ~count);
      deflated.insert(deflated.end(), text.begin() + offset, text.begin() + offset + count);
      offset += count;
    } while (offset < text.size());
    const uint32_t local = bytes.size();
    u32(bytes, 0x04034B50);
    u16(bytes, 20); u16(bytes, 0); u16(bytes, 8); u32(bytes, 0);
    u32(bytes, crc32(text)); u32(bytes, deflated.size()); u32(bytes, text.size());
    u16(bytes, name.size()); u16(bytes, 0);
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.insert(bytes.end(), deflated.begin(), deflated.end());
    u32(central, 0x02014B50);
    u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 8); u32(central, 0);
    u32(central, crc32(text)); u32(central, deflated.size()); u32(central, text.size());
    u16(central, name.size()); u16(central, 0); u16(central, 0); u16(central, 0);
    u16(central, 0); u32(central, 0); u32(central, local);
    central.insert(central.end(), name.begin(), name.end());
  }
  const uint32_t centralOffset = bytes.size();
  bytes.insert(bytes.end(), central.begin(), central.end());
  u32(bytes, 0x06054B50); u16(bytes, 0); u16(bytes, 0);
  u16(bytes, files.size()); u16(bytes, files.size());
  u32(bytes, central.size()); u32(bytes, centralOffset); u16(bytes, 0);
  return bytes;
}

std::vector<uint8_t> epubZip(const std::string& css, const std::string& html) {
  return zip({
      {"META-INF/container.xml", "<container><rootfiles><rootfile full-path=\"content.opf\" "
                                "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"},
      {"content.opf",
       "<package xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><metadata><dc:title>Fixture</dc:title>"
       "<dc:creator>Author</dc:creator></metadata><manifest>"
       "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
       "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/></manifest>"
       "<spine><itemref idref=\"c\"/></spine></package>"},
      {"chapter.xhtml", html}, {"style.css", css}});
}

class StorageRepair : public testing::Test {
  void SetUp() override { HalStorage::resetTestFiles(); }
};

TEST_F(StorageRepair, OversizeCssPersistsFallbackAndWarmOpenKeepsSections) {
  HalStorage::registerTestFile("/book.epub", epubZip(std::string(130 * 1024, ' '), "<html><body>hello</body></html>"));
  Epub first("/book.epub", "/cache");
  first.setupCacheDir();
  ASSERT_TRUE(first.load());
  CssParser rules(first.getCachePath());
  EXPECT_EQ(rules.validateCache(), CssParser::CacheStatus::Complete);
  EXPECT_EQ(rules.loadFromCache(), CssParser::CacheLoadResult::Complete);
  const std::string sentinel = first.getCachePath() + "/sections/0.bin";
  HalStorage::registerTestFile(sentinel, {1, 2, 3});
  const auto removed = HalStorage::removeDirCalls();
  const auto zipReads = HalStorage::bytesRead("/book.epub");
  Epub reopened("/book.epub", "/cache");
  ASSERT_TRUE(reopened.load());
  EXPECT_TRUE(Storage.exists(sentinel));
  EXPECT_EQ(HalStorage::removeDirCalls(), removed);
  EXPECT_EQ(HalStorage::bytesRead("/book.epub"), zipReads);
}

TEST_F(StorageRepair, FailedCssReplacementPreservesExistingSectionUntilRetrySucceeds) {
  HalStorage::registerTestFile("/book.epub", epubZip("p{font-weight:bold}", "<html><body>hello</body></html>"));
  Epub first("/book.epub", "/cache");
  first.setupCacheDir();
  ASSERT_TRUE(first.load());
  const auto cache = first.getCachePath();
  CssParser(cache).deleteCache();
  const std::string sentinel = cache + "/sections/0.bin";
  HalStorage::registerTestFile(sentinel, {1, 2, 3});
  HalStorage::failWriteOpen(cache + "/css_rules.cache.tmp");
  Epub failed("/book.epub", "/cache");
  ASSERT_TRUE(failed.load());
  EXPECT_TRUE(Storage.exists(sentinel));
  HalStorage::allowWriteOpen(cache + "/css_rules.cache.tmp");
  Epub retried("/book.epub", "/cache");
  ASSERT_TRUE(retried.load());
  EXPECT_FALSE(Storage.exists(sentinel));
}

TEST_F(StorageRepair, MetadataOnlyParseWritesNoItemsOrPerBookDirectory) {
  HalStorage::registerTestFile("/book.epub", epubZip("", "<html><body>hello</body></html>"));
  Epub book("/book.epub", "/cache");
  ASSERT_TRUE(book.loadMetadataOnly());
  EXPECT_EQ(book.getTitle(), "Fixture");
  EXPECT_EQ(book.getAuthor(), "Author");
  EXPECT_EQ(HalStorage::mkdirCalls(), 0u);
  EXPECT_EQ(HalStorage::writeOpenAttempts(book.getCachePath() + "/.items.bin"), 0u);
}

TEST_F(StorageRepair, MetadataIndexAndTitlesDoNoOffscreenCoverWork) {
  std::vector<std::string> paths;
  for (int i = 0; i < 100; ++i) {
    paths.push_back("/book" + std::to_string(i) + ".epub");
    HalStorage::registerTestFile(paths.back(), {1});
  }
  HalStorage::registerTestDirectory("/", paths);
  metadataResolutions = thumbnailConversions = 0;
  LibraryIndexBuilder builder(LibraryIndex::PATH);
  builder.begin();
  while (builder.hasWork()) builder.step();
  EXPECT_FALSE(builder.hasCoverWork());
  ASSERT_TRUE(builder.commit());
  EXPECT_EQ(metadataResolutions, 100);
  EXPECT_EQ(thumbnailConversions, 0);
  // Removing the enumeration source proves flat Titles uses the index.
  HalStorage::registerTestDirectory("/", {});
  auto titles = LibraryGrouping::loadLibraryEntries(false, true);
  ASSERT_EQ(titles.size(), 100u);
  LibraryGrouping::resolvePage(titles, 0, 7);
  EXPECT_EQ(metadataResolutions, 100);
  LibraryGrouping::resolvePage(titles, 0, 7, 80, 120);
  EXPECT_EQ(thumbnailConversions, 7);
  EXPECT_EQ(metadataResolutions, 107);
  for (int i = 7; i < 100; ++i) EXPECT_FALSE(titles[i].hasCover);
}

TEST_F(StorageRepair, IndexedTextAndExistingThumbnailAvoidMetadataParsing) {
  std::vector<LibraryGrouping::Entry> entries(1);
  entries[0].path = "/book.epub";
  entries[0].title = "Already indexed";
  entries[0].author = "Author";
  const auto thumbnail = Epub("/book.epub", "/.crosspoint").getThumbBmpPath(80, 120);
  HalStorage::registerTestFile(thumbnail, {1, 2});
  metadataResolutions = thumbnailConversions = 0;
  EXPECT_FALSE(LibraryGrouping::resolvePage(entries, 0, 1, 80, 120));
  EXPECT_EQ(entries[0].coverThumbPath, thumbnail);
  EXPECT_TRUE(entries[0].hasCover);
  EXPECT_EQ(metadataResolutions, 0);
}

class TextSink : public Print {
 public:
  std::string text;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t byte) override {
    if (text.size() == limit) return 0;
    text += static_cast<char>(byte);
    return 1;
  }
};

TEST_F(StorageRepair, ValidHtmlAvoidsSecondZipReadAndSupportsEarlyStop) {
  const std::string html = "<html><body><p>" + std::string(8000, 'a') + "</p></body></html>";
  HalStorage::registerTestFile("/book.epub", epubZip("", html));
  Epub book("/book.epub", "/cache");
  ASSERT_TRUE(book.load(true, true));
  HalStorage::registerTestFile(book.getCachePath() + "/html/0.html", {html.begin(), html.end()});
  TextSink first;
  ASSERT_TRUE(book.readSpineContentsToStream(0, first, 1024));
  EXPECT_EQ(first.text, html);
  const auto zipReads = HalStorage::bytesRead("/book.epub");
  const auto cacheReads = HalStorage::bytesRead(book.getCachePath() + "/html/0.html");
  TextSink next;
  next.limit = 17;
  ASSERT_TRUE(book.readSpineContentsToStream(0, next, 128, true));
  EXPECT_EQ(next.text, html.substr(0, 17));
  EXPECT_EQ(HalStorage::bytesRead("/book.epub"), zipReads);
  EXPECT_EQ(HalStorage::bytesRead(book.getCachePath() + "/html/0.html") - cacheReads, 128u);
}

TEST_F(StorageRepair, CorruptCachedHtmlFallsBackToCurrentArchive) {
  const std::string html = "<html><body><p>current</p></body></html>";
  HalStorage::registerTestFile("/book.epub", epubZip("", html));
  Epub book("/book.epub", "/cache");
  ASSERT_TRUE(book.load(true, true));
  HalStorage::registerTestFile(book.getCachePath() + "/html/0.html", {'b', 'a', 'd'});
  TextSink sink;
  ASSERT_TRUE(book.readSpineContentsToStream(0, sink, 128));
  EXPECT_EQ(sink.text, html);
}

TEST_F(StorageRepair, RealScannerRetainsBoundedNaturalPrefixOfHugeDirectory) {
  std::vector<std::string> paths;
  for (int i = 19999; i >= 0; --i) {
    const std::string path = "/book" + std::to_string(i) + ".epub";
    paths.push_back(path);
    HalStorage::registerTestFile(path, {});
  }
  HalStorage::registerTestDirectory("/", paths);
  LibraryScanner::ScanStats stats;
  const auto books = LibraryScanner::scanAllBooks("/", 7, true, &stats);
  ASSERT_EQ(books.size(), 7u);
  for (size_t i = 0; i < books.size(); ++i) EXPECT_EQ(books[i].path, "/book" + std::to_string(i) + ".epub");
  EXPECT_LE(stats.directoryEntries, 7u);
  EXPECT_LE(stats.directoryBytes, LibraryScanner::DIRECTORY_BYTE_BUDGET);
  EXPECT_TRUE(stats.capped);
}

TEST_F(StorageRepair, LongNameAndWideTreeScansRespectAllBudgets) {
  std::vector<std::string> dirs;
  for (int i = 0; i < 400; ++i) {
    std::string dir = "/dir" + std::to_string(i) + std::string(180, 'x');
    std::string book = dir + "/" + std::string(450, 'y') + ".epub";
    HalStorage::registerTestFile(book, {});
    HalStorage::registerTestDirectory(dir, {book});
    dirs.push_back(dir);
  }
  HalStorage::registerTestDirectory("/", dirs);
  LibraryScanner::ScanStats stats;
  const auto books = LibraryScanner::scanAllBooks("/", 2000, false, &stats);
  EXPECT_FALSE(books.empty());
  EXPECT_LE(stats.pendingDirectories, LibraryScanner::MAX_PENDING_DIRS);
  EXPECT_LE(stats.pendingBytes, LibraryScanner::DIRECTORY_BYTE_BUDGET);
  EXPECT_LE(stats.resultBytes, LibraryScanner::RESULT_BYTE_BUDGET);
  EXPECT_TRUE(stats.capped);
}

TEST(BoundedDirectorySelection, ByteBoundSelectsSamePrefixForEveryEnumerationOrder) {
  struct Item { int key; size_t bytes; };
  std::vector<Item> source{{1, 6}, {2, 6}, {3, 4}};
  do {
    std::vector<Item> retained;
    std::optional<Item> cutoff;
    size_t bytes = 0;
    for (const auto item : source) {
      retainFileEntry(retained, cutoff, item, bytes, 10, 10,
                      [](Item a, Item b) { return a.key < b.key; }, [](Item i) { return i.bytes; });
    }
    ASSERT_EQ(retained.size(), 1u);
    EXPECT_EQ(retained.front().key, 1);
    EXPECT_EQ(bytes, 6u);
  } while (std::next_permutation(source.begin(), source.end(), [](Item a, Item b) { return a.key < b.key; }));
}

std::vector<uint8_t> metadataFile(int count, const std::string& prefix = "chapter") {
  std::vector<uint8_t> bytes;
  u32(bytes, serialization::FORK_MAGIC);
  bytes.push_back(11);
  // Header ends at 13; six empty metadata strings and the series float end at 41.
  u32(bytes, 41); u16(bytes, count); u16(bytes, 0);
  for (int i = 0; i < 6; ++i) u32(bytes, 0);
  u32(bytes, 0);
  uint32_t payload = 41 + 4 * count;
  for (int i = 0; i < count; ++i) {
    u32(bytes, payload);
    payload += 4 + prefix.size() + std::to_string(i).size() + 6 + 4 + 2;
  }
  for (int i = 0; i < count; ++i) {
    const auto href = prefix + std::to_string(i) + ".xhtml";
    u32(bytes, href.size()); bytes.insert(bytes.end(), href.begin(), href.end());
    u32(bytes, (i + 1) * 100); u16(bytes, 0xFFFF);
  }
  return bytes;
}

TEST_F(StorageRepair, MetadataHotReadsAreCachedAndReloadInvalidatesThem) {
  HalStorage::registerTestFile("/cache/book.bin", metadataFile(300));
  BookMetadataCache cache("/cache");
  ASSERT_TRUE(cache.load());
  for (const int i : {299, 6, 5}) EXPECT_EQ(cache.getSpineEntry(i).cumulativeSize, (i + 1) * 100u);
  const auto bytes = HalStorage::bytesRead("/cache/book.bin");
  for (int turn = 0; turn < 100; ++turn) {
    for (const int i : {299, 6, 5}) EXPECT_EQ(cache.getSpineEntry(i).cumulativeSize, (i + 1) * 100u);
  }
  EXPECT_EQ(HalStorage::bytesRead("/cache/book.bin"), bytes);
  HalStorage::registerTestFile("/cache/book.bin", metadataFile(300, "updated"));
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.getSpineEntry(6).href, "updated6.xhtml");
}

TEST_F(StorageRepair, SharedMetadataCursorIsSafeAcrossConcurrentReaders) {
  HalStorage::registerTestFile("/cache/book.bin", metadataFile(300));
  BookMetadataCache cache("/cache");
  ASSERT_TRUE(cache.load());
  const auto read = [&cache](int start) {
    for (int n = 0; n < 1000; ++n) {
      const int index = (start + n * 29) % 300;
      const auto entry = cache.getSpineEntry(index);
      EXPECT_EQ(entry.href, "chapter" + std::to_string(index) + ".xhtml");
      EXPECT_EQ(entry.cumulativeSize, (index + 1) * 100u);
    }
  };
  std::thread first(read, 0);
  std::thread second(read, 7);
  first.join();
  second.join();
}

TEST_F(StorageRepair, CorruptStringLengthIsRejectedBeforeAllocation) {
  auto bytes = metadataFile(3);
  std::fill(bytes.begin() + 13, bytes.begin() + 17, 0xFF);
  HalStorage::registerTestFile("/cache/book.bin", bytes);
  BookMetadataCache cache("/cache");
  EXPECT_FALSE(cache.load());
}

TEST_F(StorageRepair, LinkResolutionUsesContiguousSpinePayloadAndKeepsFallbackBehavior) {
  HalStorage::registerTestFile("/cache/book.bin", metadataFile(300));
  BookMetadataCache cache("/cache");
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.resolveSpineHref("chapter299.xhtml"), 299);
  EXPECT_EQ(cache.resolveSpineHref("other/chapter298.xhtml"), 298);
  EXPECT_EQ(cache.resolveSpineHref("absent.xhtml"), -1);
}
}  // namespace
