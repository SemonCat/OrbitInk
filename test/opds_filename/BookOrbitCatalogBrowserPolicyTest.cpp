#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "BookOrbitCatalogBrowserPolicy.h"

namespace {

using BookOrbitCatalogBrowserPolicy::NAVIGATION_HISTORY_LIMIT;
using BookOrbitCatalogBrowserPolicy::NavigationHistory;

struct NavFrame {
  int page = 0;
  int selectorIndex = 0;
};

struct FakeCatalogSession {
  FakeCatalogSession(const int instance, int* destroyed) : instance(instance), destroyed(destroyed) {}
  ~FakeCatalogSession() { ++*destroyed; }

  int instance;
  int* destroyed;
};

TEST(BookOrbitCatalogDownloadPath, KeepsTheExistingNameForOneEpubChoice) {
  EXPECT_EQ(BookOrbitCatalogBrowserPolicy::downloadPath("Frank Herbert", "Dune", "101", 1),
            "/Frank Herbert - Dune.epub");
}

TEST(BookOrbitCatalogDownloadPath, GivesEachEpubChoiceAStableDistinctSafePath) {
  const std::string first = BookOrbitCatalogBrowserPolicy::downloadPath("A/B", "Dune: Messiah", "101", 2);
  const std::string second = BookOrbitCatalogBrowserPolicy::downloadPath("A/B", "Dune: Messiah", "202", 2);

  EXPECT_NE(first, second);
  EXPECT_EQ(first, "/A_B - Dune_ Messiah [101].epub");
  EXPECT_EQ(second, "/A_B - Dune_ Messiah [202].epub");
  EXPECT_EQ(first, BookOrbitCatalogBrowserPolicy::downloadPath("A/B", "Dune: Messiah", "101", 2));
  EXPECT_EQ(second, BookOrbitCatalogBrowserPolicy::downloadPath("A/B", "Dune: Messiah", "202", 2));
  EXPECT_EQ(first.find('/', 1), std::string::npos);
  EXPECT_EQ(second.find('/', 1), std::string::npos);
  EXPECT_LE(first.size(), 106u);  // root slash + the existing 100-byte stem budget + ".epub"
  EXPECT_LE(second.size(), 106u);
}

TEST(BookOrbitCatalogDownloadPath, UnsafeOrLongFileIdsRemainStableSafeAndDistinct) {
  const std::string firstId = "../variant/" + std::string(80, 'a');
  const std::string secondId = "../variant/" + std::string(79, 'a') + "b";
  const std::string first = BookOrbitCatalogBrowserPolicy::downloadPath("Author", "Title", firstId, 2);
  const std::string second = BookOrbitCatalogBrowserPolicy::downloadPath("Author", "Title", secondId, 2);

  EXPECT_NE(first, second);
  EXPECT_EQ(first, BookOrbitCatalogBrowserPolicy::downloadPath("Author", "Title", firstId, 2));
  EXPECT_EQ(first.find('/', 1), std::string::npos);
  EXPECT_EQ(second.find('/', 1), std::string::npos);
}

TEST(BookOrbitCatalogNavigationHistory, RetainsOnlyTheNewestFramesAndPopsThemInNavigationOrder) {
  NavigationHistory<NavFrame> history;
  for (size_t page = 0; page < NAVIGATION_HISTORY_LIMIT + 3; ++page) {
    history.push({static_cast<int>(page), static_cast<int>(page * 10)});
  }

  ASSERT_EQ(history.size(), NAVIGATION_HISTORY_LIMIT);
  for (int page = static_cast<int>(NAVIGATION_HISTORY_LIMIT + 2); page >= 3; --page) {
    NavFrame frame;
    ASSERT_TRUE(history.popBack(frame));
    EXPECT_EQ(frame.page, page);
    EXPECT_EQ(frame.selectorIndex, page * 10);
  }
  NavFrame frame;
  EXPECT_FALSE(history.popBack(frame));
}

TEST(BookOrbitCatalogNavigationHistory, PreviousAndNextFramesRemainLifoAtTheCapacityBoundary) {
  NavigationHistory<NavFrame> history;
  for (size_t page = 0; page < NAVIGATION_HISTORY_LIMIT - 1; ++page) {
    history.push({static_cast<int>(page), 0});
  }

  history.push({7, 21});  // page 7 before selecting Next to page 8
  history.push({8, 0});   // page 8 before selecting Previous back to page 7; evicts the oldest

  NavFrame frame;
  ASSERT_TRUE(history.popBack(frame));
  EXPECT_EQ(frame.page, 8);  // Back from page 7 returns to the page left via Previous.
  ASSERT_TRUE(history.popBack(frame));
  EXPECT_EQ(frame.page, 7);  // The preceding Next transition is still immediately below it.
  EXPECT_EQ(frame.selectorIndex, 21);
  EXPECT_EQ(history.size(), NAVIGATION_HISTORY_LIMIT - 2);
}

TEST(BookOrbitCatalogSessionCache, ReusesOnlyWithinTheConfiguredServerScope) {
  BookOrbitCatalogBrowserPolicy::SingleServerSessionCache<FakeCatalogSession> cache;
  int created = 0;
  int destroyed = 0;
  const auto makeSession = [&] { return std::make_unique<FakeCatalogSession>(++created, &destroyed); };

  FakeCatalogSession* first = cache.get("https://books.example.test/api/v1/koreader", makeSession);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->instance, 1);
  EXPECT_EQ(cache.get("https://books.example.test/api/v1/koreader", makeSession)->instance, 1);
  EXPECT_EQ(created, 1) << "root and books requests must retain one connection-owning session";
  EXPECT_EQ(destroyed, 0);

  FakeCatalogSession* other = cache.get("https://other.example.test/api/v1/koreader", makeSession);
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(other->instance, 2);
  EXPECT_EQ(created, 2);
  EXPECT_EQ(destroyed, 1) << "the original server's TLS/connection state must be discarded first";
  EXPECT_EQ(cache.get("https://other.example.test/api/v1/koreader", makeSession)->instance, 2);

  cache.clear();
  EXPECT_EQ(destroyed, 2);
}

}  // namespace
