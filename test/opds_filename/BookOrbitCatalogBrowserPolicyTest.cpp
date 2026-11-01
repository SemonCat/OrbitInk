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

TEST(BookOrbitCatalogBrowserPolicy, MapsEveryServerStatusAndUnknownFallback) {
  using namespace BookOrbitCatalogBrowserPolicy;
  EXPECT_EQ(readState("rereading"), ReadState::Reading);
  EXPECT_EQ(readState("skimmed"), ReadState::Finished);
  EXPECT_EQ(readState("on_hold"), ReadState::OnHold);
  EXPECT_EQ(readState("abandoned"), ReadState::Abandoned);
  EXPECT_EQ(readState("future_status"), ReadState::Other);
  EXPECT_TRUE(matchesReadFilter("rereading", "reading"));
  EXPECT_TRUE(matchesReadFilter("on_hold", "reading"));
  EXPECT_TRUE(matchesReadFilter("skimmed", "finished"));
  EXPECT_TRUE(matchesReadFilter("abandoned", "finished"));
  EXPECT_FALSE(matchesReadFilter("read", "reading"));
  EXPECT_TRUE(matchesReadFilter("want_to_read", "unread"));
}

TEST(BookOrbitCatalogBrowserPolicy, PagingMovesAtEdgesAndNeverWrapsTheLastPage) {
  using namespace BookOrbitCatalogBrowserPolicy;
  auto next = moveInList(0, 20, 21, true, 1);
  EXPECT_TRUE(next.fetch);
  EXPECT_EQ(next.page, 1);
  EXPECT_EQ(next.index, 0);
  auto previous = moveInList(1, 0, 21, true, -1);
  EXPECT_TRUE(previous.fetch);
  EXPECT_EQ(previous.page, 0);
  EXPECT_EQ(previous.index, -1);
  auto end = moveInList(2, 6, 7, false, 1);
  EXPECT_FALSE(end.fetch);
  EXPECT_EQ(end.index, 6);
  auto begin = moveInList(0, 0, 21, true, -1);
  EXPECT_FALSE(begin.fetch);
  EXPECT_EQ(begin.index, 0);
  auto screen = moveInList(0, 18, 21, true, 6);
  EXPECT_TRUE(screen.fetch);
  EXPECT_EQ(screen.page, 1);
}

TEST(BookOrbitCatalogBrowserPolicy, WrapsUnspacedCjkAndMixedTitlesWithinMeasuredWidth) {
  const auto measure = [](const std::string& text) {
    int glyphs = 0;
    for (const unsigned char c : text) if ((c & 0xc0) != 0x80) ++glyphs;
    return glyphs;
  };
  EXPECT_EQ(BookOrbitCatalogBrowserPolicy::wrapTitle("星光圖書館長夜", 4, 3, measure),
            (std::vector<std::string>{"星光圖書", "館長夜"}));
  EXPECT_EQ(BookOrbitCatalogBrowserPolicy::wrapTitle("North 星光圖書", 6, 3, measure),
            (std::vector<std::string>{"North", "星光圖書"}));
  const auto clipped = BookOrbitCatalogBrowserPolicy::wrapTitle("星光圖書館長夜", 4, 1, measure);
  ASSERT_EQ(clipped.size(), 1u);
  EXPECT_EQ(clipped[0], "星...");
  EXPECT_LE(measure(clipped[0]), 4);
}
