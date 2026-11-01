// Owner-scoped offline catalog cache (src/util/BookOrbitCatalogCache).
#include <gtest/gtest.h>

#include <string>

#include "BookOrbitCatalogCache.h"
#include "HalStorage.h"

namespace {

void digest2(const uint8_t* data, const size_t len, uint8_t out[32]) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 32; i++) {
    h ^= static_cast<uint32_t>(i + 1) * 2654435761u;
    h *= 16777619u;
    out[i] = static_cast<uint8_t>((h >> 13) & 0xFF);
  }
}

BookOrbitOwnerId ownerOf(const char* user) { return BookOrbitOwnerId::derive("https://bo.example", user, &digest2); }

using BookOrbitCatalogCache::Cache;
using Client = BookOrbitCatalogClient;

Client::BookPage samplePage() {
  Client::BookPage page;
  page.hasNext = true;
  page.total = 46;
  Client::BookSummary a;
  a.id = "101";
  a.title = "沙丘";
  a.authors = "法蘭克·赫伯特";
  a.progressPercent = 42.5f;
  a.readStatus = "reading";
  a.seriesName = "星光圖書館";
  a.seriesIndex = "2.5";
  Client::BookSummary b;
  b.id = "102";
  b.title = "Second";
  page.items = {a, b};
  return page;
}

class CatalogCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { testStorage().reset(); }
};

TEST_F(CatalogCacheTest, RootAndBookPagesRoundTrip) {
  Cache cache(ownerOf("alice"), &digest2);
  ASSERT_TRUE(cache.saveRoot({{"all-books", "全部書籍"}, {"authors", "作者"}}, 1760000000u));
  Client::BookQuery q;
  q.sort = "title";
  ASSERT_TRUE(cache.saveBooks(q, samplePage(), 1760000100u));

  std::vector<Client::Section> root;
  BookOrbitCatalogCache::Meta meta;
  ASSERT_TRUE(cache.loadRoot(root, meta));
  ASSERT_EQ(root.size(), 2u);
  EXPECT_EQ(root[0].title, "全部書籍");
  EXPECT_EQ(meta.savedAt, 1760000000u);

  Client::BookPage page;
  ASSERT_TRUE(cache.loadBooks(q, page, meta));
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_TRUE(page.hasNext);
  EXPECT_EQ(page.items[0].id, "101");
  EXPECT_EQ(page.total, 46u);
  EXPECT_EQ(page.items[0].seriesName, "星光圖書館");
  EXPECT_EQ(page.items[0].seriesIndex, "2.5");
  EXPECT_NEAR(page.items[0].progressPercent, 42.5f, 0.01f);
  EXPECT_LT(page.items[1].progressPercent, 0.0f);
}

TEST_F(CatalogCacheTest, ADifferentQueryOrAccountNeverReadsTheCache) {
  Cache alice(ownerOf("alice"), &digest2);
  Client::BookQuery q;
  q.sort = "title";
  ASSERT_TRUE(alice.saveBooks(q, samplePage(), 1u));
  Client::BookQuery other = q;
  other.page = 1;
  Client::BookPage page;
  BookOrbitCatalogCache::Meta meta;
  EXPECT_FALSE(alice.loadBooks(other, page, meta));

  Cache bob(ownerOf("bob"), &digest2);
  EXPECT_FALSE(bob.loadBooks(q, page, meta)) << "another account's cache is never read";
  EXPECT_NE(alice.directory(), bob.directory());
}

TEST_F(CatalogCacheTest, ACorruptCacheIsRepairedBySavingTheSameContentAgain) {
  Cache cache(ownerOf("alice"), &digest2);
  for (const int corrupt : {0, 1}) {  // 0: the checksum byte, 1: one savedAt bit
    testStorage().reset();
    ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}}, 5u));
    std::string path;
    for (const auto& [p, bytes] : testStorage().files) {
      if (p.find("root") != std::string::npos && p.find(".tmp") == std::string::npos) path = p;
    }
    ASSERT_FALSE(path.empty());
    auto& bytes = testStorage().files[path];
    const size_t savedAtOff = 4 + 1 + BookOrbitOwnerId::BYTES + 1;
    if (corrupt == 0)
      bytes.back() ^= 0x01;
    else
      bytes[savedAtOff] ^= 0x01;
    std::vector<Client::Section> root;
    BookOrbitCatalogCache::Meta meta;
    ASSERT_FALSE(cache.loadRoot(root, meta)) << "corruption is detected";
    // A healthy fetch saves the SAME payload: it must replace the corrupt file, not skip it.
    ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}}, 6u));
    ASSERT_TRUE(cache.loadRoot(root, meta)) << "case " << corrupt << ": the cache stayed broken";
    ASSERT_EQ(root.size(), 1u);
    EXPECT_EQ(meta.savedAt, 6u);
  }
}

TEST_F(CatalogCacheTest, ACorruptFileIsIgnoredNotTrusted) {
  Cache cache(ownerOf("alice"), &digest2);
  ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}}, 1u));
  for (auto& [path, bytes] : testStorage().files) {
    if (path.find("root") != std::string::npos) bytes[bytes.size() / 2] ^= 0x10;
  }
  std::vector<Client::Section> root;
  BookOrbitCatalogCache::Meta meta;
  EXPECT_FALSE(cache.loadRoot(root, meta));
}

TEST_F(CatalogCacheTest, TheCacheIsBoundedAndEvictsItsOldestFile) {
  Cache cache(ownerOf("alice"), &digest2);
  for (int i = 0; i < 20; i++) {
    Client::BookQuery q;
    q.page = i;
    ASSERT_TRUE(cache.saveBooks(q, samplePage(), 1000u + static_cast<uint32_t>(i)));
  }
  size_t files = 0;
  for (const auto& [path, bytes] : testStorage().files) {
    (void)bytes;
    if (path.rfind(cache.directory() + "/", 0) == 0) files++;
  }
  EXPECT_EQ(files, BookOrbitCatalogCache::MAX_FILES);
  Client::BookQuery newest;
  newest.page = 19;
  Client::BookQuery oldest;
  oldest.page = 0;
  Client::BookPage page;
  BookOrbitCatalogCache::Meta meta;
  EXPECT_TRUE(cache.loadBooks(newest, page, meta));
  EXPECT_FALSE(cache.loadBooks(oldest, page, meta));
}

TEST_F(CatalogCacheTest, IdenticalContentIsNotRewrittenEvenWithANewSaveTime) {
  Cache cache(ownerOf("alice"), &digest2);
  ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}}, 5u));
  const int renames = testStorage().renameCount;
  // Every visit carries a new savedAt; the content is the same, so no SD write happens.
  ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}}, 9999u));
  EXPECT_EQ(testStorage().renameCount, renames);
  std::vector<Client::Section> root;
  BookOrbitCatalogCache::Meta meta;
  ASSERT_TRUE(cache.loadRoot(root, meta));
  EXPECT_EQ(meta.savedAt, 5u) << "the first save time is kept when nothing changed";
  // Changed content IS written.
  ASSERT_TRUE(cache.saveRoot({{"all-books", "All"}, {"recent", "New"}}, 10000u));
  EXPECT_GT(testStorage().renameCount, renames);
}

TEST_F(CatalogCacheTest, NoCredentialsMeansNoCache) {
  Cache cache(BookOrbitOwnerId{}, &digest2);
  EXPECT_FALSE(cache.saveRoot({{"a", "b"}}, 1u));
  EXPECT_TRUE(testStorage().files.empty());
}

}  // namespace

TEST_F(CatalogCacheTest, ExistingVersionOneBookPagesStillOpenOffline) {
  Cache cache(ownerOf("alice"), &digest2);
  Client::BookPage original;
  Client::BookSummary book;
  book.id = "101";
  book.title = "Legacy local catalog";
  original.items.push_back(book);
  Client::BookQuery query;
  ASSERT_TRUE(cache.saveBooks(query, original, 123u));
  for (auto& [path, bytes] : testStorage().files) {
    if (path.find(".bin") == std::string::npos) continue;
    constexpr size_t headerBytes = 4 + 1 + BookOrbitOwnerId::BYTES + 1 + 4 + 1 + 2;
    bytes[4] = 1;
    bytes.erase(bytes.begin() + headerBytes, bytes.begin() + headerBytes + 4);  // v2 total
    bytes.erase(bytes.end() - 3, bytes.end() - 1);  // v2 empty seriesName/index
    uint8_t sum = 0x69;
    for (size_t i = 0; i + 1 < bytes.size(); ++i)
      sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ bytes[i]);
    bytes.back() = sum;
  }
  Client::BookPage loaded;
  BookOrbitCatalogCache::Meta meta;
  ASSERT_TRUE(cache.loadBooks(query, loaded, meta));
  ASSERT_EQ(loaded.items.size(), 1u);
  EXPECT_EQ(loaded.items[0].title, book.title);
  EXPECT_TRUE(loaded.items[0].seriesName.empty());
  EXPECT_EQ(loaded.total, 0u);
}
