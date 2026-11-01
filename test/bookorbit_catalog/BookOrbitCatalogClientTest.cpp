// Host tests for the REAL lib/KOReaderSync/BookOrbitCatalogClient.cpp (compiled directly, not
// reimplemented).
//
// Two groups: the JSON protocol methods (fetchRootSections/fetchBooks/fetchSectionEntries/
// fetchBookDetail) against a fake, in-memory KOSyncTransport -- no network, WiFi or TLS -- and
// downloadFile()'s SD-side atomicity (temp+backup+promote, short write, close/flush failure,
// rename failure, stale-backup recovery/refusal, aborted transfer) against an injected FileFetcher
// plus the same in-memory, failure-injectable HalStorage stub BookOrbitStatsQueue's own tests use.

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "BookOrbitCatalogClient.h"
#include "BookOrbitJsonKeyDropper.h"
#include "BookOrbitCredentialStore.h"
#include "HalStorage.h"
#include "KOSyncTransport.h"
#include "Logging.h"

namespace {

using Client = BookOrbitCatalogClient;

// ---- Fake KOSyncTransport (JSON methods) -------------------------------------------------------

class FakeKOSyncTransport final : public KOSyncTransport {
 public:
  struct ScriptedResponse {
    int httpCode;
    std::string body;
  };
  struct RecordedRequest {
    std::string url;
    std::string method;
    std::string payload;
  };

  explicit FakeKOSyncTransport(std::vector<ScriptedResponse> responses) : responses_(std::move(responses)) {}

  bool begin(const std::string& url) override {
    lastUrl_ = url;
    urls.push_back(url);
    return !url.empty();
  }
  void addHeader(const std::string& name, const std::string& value) override { headers.emplace_back(name, value); }

  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                  const AbortCallback&) override {
    requests.push_back(
        {lastUrl_, method, payload ? std::string(reinterpret_cast<const char*>(payload), payloadLen) : ""});
    if (nextResponse_ >= responses_.size()) return -1;
    const ScriptedResponse& resp = responses_[nextResponse_++];
    lastBody_ = resp.body;
    if (onData && !resp.body.empty()) {
      // chunkSize > 0 delivers the body the way a socket does: in arbitrary pieces.
      const size_t step = chunkSize > 0 ? chunkSize : resp.body.size();
      for (size_t at = 0; at < resp.body.size(); at += step) {
        onData(reinterpret_cast<const uint8_t*>(resp.body.data()) + at, std::min(step, resp.body.size() - at));
      }
    }
    return resp.httpCode;
  }

  const std::string& getString() const override { return lastBody_; }
  void end() override {}

  size_t chunkSize = 0;
  std::vector<std::string> urls;
  std::vector<RecordedRequest> requests;
  std::vector<std::pair<std::string, std::string>> headers;

 private:
  std::vector<ScriptedResponse> responses_;
  size_t nextResponse_ = 0;
  std::string lastUrl_;
  std::string lastBody_;
};

std::unique_ptr<Client::Session> makeJsonSession(std::vector<FakeKOSyncTransport::ScriptedResponse> responses,
                                                 FakeKOSyncTransport** outFake = nullptr) {
  auto* fake = new FakeKOSyncTransport(std::move(responses));
  if (outFake) *outFake = fake;
  return std::make_unique<Client::Session>(std::unique_ptr<KOSyncTransport>(fake));
}

class BookOrbitCatalogClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    BOOKORBIT_STORE.resetToDefaults();
    BOOKORBIT_STORE.setTestBaseUrl("https://books.example.test/api/v1/koreader");
    ESP.freeHeap = 0xFFFFFFFFu;
    ESP.maxAllocHeap = 0xFFFFFFFFu;
    testStorage().reset();
  }
};

// ---- fetchRootSections ---------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, RootSectionsParsesIdAndTitle) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"sections":[{"section":"all-books","title":"All Books"},
                                                          {"section":"authors","title":"Authors"}]})"}},
                                 &fake);
  std::vector<Client::Section> sections;
  ASSERT_EQ(session->fetchRootSections(sections), Client::OK);
  ASSERT_EQ(sections.size(), 2u);
  EXPECT_EQ(sections[0].id, "all-books");
  EXPECT_EQ(sections[0].title, "All Books");
  EXPECT_EQ(sections[1].id, "authors");
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_EQ(fake->urls[0], "https://books.example.test/api/v1/koreader/plugin/catalog/root");
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsSkipsEntriesMissingRequiredFields) {
  auto session = makeJsonSession({{200, R"({"sections":[{"title":"No id here"},{"section":"ok","title":"Ok"}]})"}});
  std::vector<Client::Section> sections;
  ASSERT_EQ(session->fetchRootSections(sections), Client::OK);
  ASSERT_EQ(sections.size(), 1u);
  EXPECT_EQ(sections[0].id, "ok");
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsRejectsWrongShapeAsJsonError) {
  auto session = makeJsonSession({{200, R"({"notSections":[]})"}});
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsRejectsMalformedJson) {
  auto session = makeJsonSession({{200, "{not json"}});
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsBodyIsBoundedRegardlessOfServerSize) {
  // One legitimate section followed by ~8KB of padding in an unused field -- comfortably past
  // MAX_ROOT_BODY (2048). The accumulation cap must truncate the body before the JSON closes, so
  // the result is a parse failure, never a multi-kilobyte buffer silently retained in RAM.
  std::string body = R"({"sections":[{"section":"ok","title":"Ok"}],"pad":")";
  body += std::string(8192, 'x');
  body += "\"}";
  auto session = makeJsonSession({{200, body}});
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsNoCredentialsShortCircuits) {
  BOOKORBIT_STORE.setTestCredentials(false);
  auto session = makeJsonSession({});
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::NO_CREDENTIALS);
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsInsufficientHeapReturnsLowMemory) {
  ESP.freeHeap = 100;
  auto session = makeJsonSession({{200, R"({"sections":[]})"}});
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::LOW_MEMORY);
}

TEST_F(BookOrbitCatalogClientTest, RootSectionsTransportFailureIsNetworkError) {
  auto session = makeJsonSession({});  // no scripted responses -> sendRequest returns -1
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::NETWORK_ERROR);
}

// ---- fetchBooks -----------------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, FetchBooksParsesItemsAndJoinsAuthors) {
  auto session = makeJsonSession(
      {{200, R"({"page":1,"total":1,"size":20,"items":[{"id":"b1","title":"Dune","authors":["Frank Herbert"]}]})"}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].id, "b1");
  EXPECT_EQ(page.items[0].title, "Dune");
  EXPECT_EQ(page.items[0].authors, "Frank Herbert");
  EXPECT_FALSE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksTruncatesTitleAtUtf8CodepointBoundaryNotRawByte) {
  // U+66F8 ("book") is 3 bytes in UTF-8; MAX_TITLE_CHARS (200) is not a multiple of 3, so a raw
  // byte truncation at exactly 200 bytes would cut the 67th codepoint in half. 100 repetitions
  // (300 bytes) is comfortably past the cap.
  std::string title;
  for (int i = 0; i < 100; i++) title += "\xe6\x9b\xb8";
  const std::string body = R"({"items":[{"id":"b1","title":")" + title + "\"}]}";
  auto session = makeJsonSession({{200, body}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(page.items.size(), 1u);
  const std::string& result = page.items[0].title;
  EXPECT_LE(result.size(), 200u);
  EXPECT_EQ(result.size() % 3, 0u) << "must not cut mid-codepoint, got " << result.size() << " bytes";
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksAuthorJoinNeverOvershootsOrEndsWithATrailingSeparator) {
  std::string authorsJson;
  for (int i = 0; i < 100; i++) {
    if (i) authorsJson += ",";
    authorsJson += "\"Author" + std::to_string(i) + "\"";
  }
  const std::string body = R"({"items":[{"id":"b1","title":"T","authors":[)" + authorsJson + "]}]}";
  auto session = makeJsonSession({{200, body}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(page.items.size(), 1u);
  const std::string& joined = page.items[0].authors;
  EXPECT_LE(joined.size(), 200u);
  EXPECT_FALSE(joined.size() >= 2 && joined.compare(joined.size() - 2, 2, ", ") == 0) << joined;
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksHasNextTrueWhenMorePagesRemain) {
  auto session = makeJsonSession({{200, R"({"page":1,"total":50,"size":20,"items":[{"id":"b1","title":"A"}]})"}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  EXPECT_TRUE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksUsesOneBasedWirePagesAndKeepsInternalNavigationZeroBased) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"page":1,"total":21,"size":20,"items":[{"id":"b1","title":"First"}]})"},
                                  {200, R"({"page":2,"total":21,"size":20,"items":[{"id":"b2","title":"Last"}]})"}},
                                 &fake);

  Client::BookQuery query;
  Client::BookPage first;
  query.page = 0;
  ASSERT_EQ(session->fetchBooks(query, first), Client::OK);
  EXPECT_TRUE(first.hasNext);

  Client::BookPage second;
  query.page = 1;
  ASSERT_EQ(session->fetchBooks(query, second), Client::OK);
  EXPECT_FALSE(second.hasNext);

  ASSERT_EQ(fake->urls.size(), 2u);
  EXPECT_NE(fake->urls[0].find("?page=1&size=20"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[1].find("?page=2&size=20"), std::string::npos) << fake->urls[1];
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksInfersHasNextFromFullPageWhenTotalsAbsent) {
  std::string body = R"({"items":[)";
  for (int i = 0; i < Client::PAGE_SIZE; i++) {
    if (i) body += ",";
    body += R"({"id":"b)" + std::to_string(i) + R"(","title":"T"})";
  }
  body += "]}";
  auto session = makeJsonSession({{200, body}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  EXPECT_EQ(page.items.size(), static_cast<size_t>(Client::PAGE_SIZE));
  EXPECT_TRUE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksNeverExceedsPageSizeEvenIfServerSendsMore) {
  std::string body = R"({"items":[)";
  for (int i = 0; i < Client::PAGE_SIZE + 5; i++) {
    if (i) body += ",";
    body += R"({"id":"b)" + std::to_string(i) + R"(","title":"T"})";
  }
  body += "]}";
  auto session = makeJsonSession({{200, body}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  EXPECT_EQ(page.items.size(), static_cast<size_t>(Client::PAGE_SIZE));
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksRejectsWrongShapeAsJsonError) {
  auto session = makeJsonSession({{200, R"({"notItems":[]})"}});
  Client::BookQuery query;
  Client::BookPage page;
  EXPECT_EQ(session->fetchBooks(query, page), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksBuildsPercentEncodedSearchQuery) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"items":[]})"}}, &fake);
  Client::BookQuery query;
  query.searchText = "a & b";
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_NE(fake->urls[0].find("q=a%20%26%20b"), std::string::npos) << fake->urls[0];
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksSendsLibraryCollectionSmartScopeSortAndReadStatusFilters) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"items":[]})"}}, &fake);
  Client::BookQuery query;
  query.libraryId = "7";
  query.collectionId = "9";
  query.smartScopeId = "11";
  query.sort = "recently_read";
  query.readStatus = "reading";
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_NE(fake->urls[0].find("libraryId=7"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[0].find("collectionId=9"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[0].find("smartScopeId=11"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[0].find("sort=recently_read"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[0].find("readStatus=reading"), std::string::npos) << fake->urls[0];
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksOmitsLibraryCollectionSmartScopeSortReadStatusWhenUnset) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"items":[]})"}}, &fake);
  Client::BookQuery query;  // every new field left at its default ("")
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_EQ(fake->urls[0].find("libraryId="), std::string::npos) << fake->urls[0];
  EXPECT_EQ(fake->urls[0].find("collectionId="), std::string::npos) << fake->urls[0];
  EXPECT_EQ(fake->urls[0].find("smartScopeId="), std::string::npos) << fake->urls[0];
  EXPECT_EQ(fake->urls[0].find("sort="), std::string::npos) << fake->urls[0];
  EXPECT_EQ(fake->urls[0].find("readStatus="), std::string::npos) << fake->urls[0];
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksAuthFailureMapsTo401) {
  auto session = makeJsonSession({{401, R"({"message":"nope"})"}});
  Client::BookQuery query;
  Client::BookPage page;
  EXPECT_EQ(session->fetchBooks(query, page), Client::AUTH_FAILED);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksServerErrorMapsTo500) {
  auto session = makeJsonSession({{500, ""}});
  Client::BookQuery query;
  Client::BookPage page;
  EXPECT_EQ(session->fetchBooks(query, page), Client::SERVER_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksUnsupportedOn404) {
  auto session = makeJsonSession({{404, ""}});
  Client::BookQuery query;
  Client::BookPage page;
  EXPECT_EQ(session->fetchBooks(query, page), Client::UNSUPPORTED);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksBadRequestOn400) {
  auto session = makeJsonSession({{400, ""}});
  Client::BookQuery query;
  Client::BookPage page;
  EXPECT_EQ(session->fetchBooks(query, page), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksBodyIsBoundedRegardlessOfServerSize) {
  std::string body = R"({"items":[{"id":"b1","title":"legit"})";
  body += std::string(1, ',');
  body += R"({"id":"pad","title":")";
  body += std::string(20000, 'y');
  body += R"("}]})";
  auto session = makeJsonSession({{200, body}});
  Client::BookQuery query;
  Client::BookPage page;
  // One item above the retained cap fails the request. Never present a partial fixed-offset
  // server page as complete: the browser keeps its previous page available for a safe retry.
  ASSERT_EQ(session->fetchBooks(query, page), Client::JSON_ERROR);
  EXPECT_TRUE(page.items.empty());
}

// ---- BookOrbitJsonKeyDropper ------------------------------------------------------------------

namespace {
std::string dropKeys(const std::string& json, size_t chunk, size_t cap = 1 << 20) {
  static constexpr const char* KEYS[] = {"description", "related"};
  std::string out;
  BookOrbitJsonKeyDropper dropper(KEYS, 2, out, cap);
  for (size_t at = 0; at < json.size(); at += chunk) {
    dropper.feed(reinterpret_cast<const uint8_t*>(json.data()) + at, std::min(chunk, json.size() - at));
  }
  return out;
}
}  // namespace

TEST(BookOrbitJsonKeyDropperTest, ReplacesDroppedValuesOfEveryShapeAtAnyChunkBoundary) {
  const std::string in =
      R"({"id":1,"description":"a \"quoted\" } ] { [ , : \\ tail","files":[{"id":2,"related":{"x":[1,{"y":"}"}]}}],)"
      R"("related":[{"description":"nested"}],"n":{"description":12.5},"t":"description","z":[true,"description"]})";
  const std::string want =
      R"({"id":1,"description":null,"files":[{"id":2,"related":null}],"related":null,"n":{"description":null},)"
      R"("t":"description","z":[true,"description"]})";
  for (size_t chunk = 1; chunk <= in.size(); chunk++) {
    ASSERT_EQ(dropKeys(in, chunk), want) << "chunk=" << chunk;
  }
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, want));
}

TEST(BookOrbitJsonKeyDropperTest, KeepsValuesThatOnlyLookLikeDroppedKeysAndPrimitivesBeforeWhitespace) {
  // Whitespace between ':' and a dropped value is consumed with the value.
  EXPECT_EQ(dropKeys(R"({ "description" : 42 , "k":"v"})", 3), R"({ "description" :null , "k":"v"})");
  EXPECT_EQ(dropKeys(R"(["description",{"descriptions":"keep"}])", 2), R"(["description",{"descriptions":"keep"}])");
}

TEST(BookOrbitJsonKeyDropperTest, CapBoundsRetainedBytesAndReportsTruncation) {
  static constexpr const char* KEYS[] = {"description"};
  std::string out;
  BookOrbitJsonKeyDropper dropper(KEYS, 1, out, 8);
  const std::string in = R"({"description":"0123456789","a":1})";
  dropper.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  EXPECT_EQ(out.size(), 8u);
  EXPECT_TRUE(dropper.truncated());
}

// ---- fetchSectionEntries ----------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesParsesFacetsAndExplicitHasNext) {
  auto session =
      makeJsonSession({{200, R"({"hasNext":true,"items":[{"id":"a1","title":"Frank Herbert","count":3}]})"}});
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("authors", 0, page), Client::OK);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].id, "a1");
  EXPECT_EQ(page.items[0].bookCount, 3u);
  EXPECT_TRUE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesKeepsAWholeSixtyEntryServerPage) {
  // Authors/series pages are 60 entries regardless of `size`; the next request asks for the
  // server's next page, so every entry of this one must be kept (booksHref values are dropped).
  std::string body = R"({"section":"authors","items":[)";
  for (int i = 0; i < 60; i++) {
    const std::string name = "作者名稱" + std::to_string(i);
    body += std::string(i ? "," : "") + R"({"id":")" + name + R"(","title":")" + name +
            R"(","section":"authors","count":)" + std::to_string(i + 1) +
            R"(,"booksHref":"/api/v1/koreader/plugin/catalog/books?author=%E4%BD%9C%E8%80%85%E5%90%8D%E7%A8%B1)"
            R"(%E4%BD%9C%E8%80%85%E5%90%8D%E7%A8%B1%E4%BD%9C%E8%80%85&sort=title"})";
  }
  body += R"(],"page":1,"hasNext":true,"hasPrevious":false,"nextUrl":"/api/v1/koreader/plugin/catalog/sections/authors?page=2"})";
  ASSERT_GT(body.size(), 12288u);
  auto session = makeJsonSession({{200, body}});
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("authors", 0, page), Client::OK);
  ASSERT_EQ(page.items.size(), 60u);
  EXPECT_EQ(page.items[59].title, "作者名稱59");
  EXPECT_EQ(page.items[59].bookCount, 60u);
  EXPECT_TRUE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesStreamsAllItemsOfAPageOverTheRetainedCap) {
  // 60 long CJK author names (40 characters: the longest a name-keyed id may be, 128 bytes) exceed
  // the whole-page cap. Streaming one item at a time keeps all 60 and the real hasNext flag.
  std::string body = R"({"section":"authors","items":[)";
  for (int i = 0; i < 60; i++) {
    std::string name;
    for (int c = 0; c < 40; c++) name += "龘";
    name += std::to_string(10 + i);
    body += std::string(i ? "," : "") + R"({"id":")" + name + R"(","title":")" + name + R"(","section":"authors","count":2})";
  }
  body += R"(],"page":1,"hasNext":false})";
  ASSERT_GT(body.size(), 16384u);
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, body}}, &fake);
  fake->chunkSize = 1000;
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("authors", 0, page), Client::OK);
  ASSERT_EQ(page.items.size(), 60u);
  EXPECT_EQ(page.items[0].bookCount, 2u);
  EXPECT_FALSE(page.hasNext);
}

TEST(BookOrbitJsonKeyDropperTest, RecordsTheLastCompleteTopLevelArrayElement) {
  static constexpr const char* KEYS[] = {"skip"};
  std::string out;
  BookOrbitJsonKeyDropper dropper(KEYS, 1, out, 38);
  const std::string in = R"({"items":[{"a":[1,2]},{"b":{"c":3}},{"d":"0123456789"}],"hasNext":true})";
  dropper.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  ASSERT_TRUE(dropper.truncated());
  EXPECT_EQ(out.substr(0, dropper.lastTopLevelElementEnd()), R"({"items":[{"a":[1,2]},{"b":{"c":3}})");
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesFiltersSeriesByTheirNumericSeriesId) {
  auto session = makeJsonSession({{200, R"({"section":"series","items":[)"
                                        R"({"id":"series:42","title":"Dune","section":"series","count":6,"seriesId":42},)"
                                        R"({"id":"a1","title":"Plain","count":1}],"hasNext":false})"}});
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("series", 0, page), Client::OK);
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].id, "42");
  EXPECT_EQ(page.items[1].id, "a1");
  EXPECT_FALSE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesWithoutHasNextHasNoNextPage) {
  std::string body = R"({"items":[)";
  for (int i = 0; i < 20; i++) body += std::string(i ? "," : "") + R"({"id":"c)" + std::to_string(i) + R"(","title":"C"})";
  body += "]}";
  auto session = makeJsonSession({{200, body}});
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("collections", 0, page), Client::OK);
  EXPECT_EQ(page.items.size(), 20u);
  EXPECT_FALSE(page.hasNext);
}

TEST_F(BookOrbitCatalogClientTest, FetchBooksReportsTotalAndSeriesPosition) {
  auto session = makeJsonSession({{200, R"({"page":1,"total":57,"size":20,"hasNext":true,"items":[)"
                                        R"({"id":1,"title":"A","seriesName":"星光","seriesIndex":3,"thumbnailUrl":"/x"},)"
                                        R"({"id":2,"title":"B","seriesName":"S","seriesIndex":"1b"},)"
                                        R"({"id":3,"title":"C","seriesName":null,"seriesIndex":null}]})"}});
  Client::BookQuery query;
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks(query, page), Client::OK);
  EXPECT_EQ(page.total, 57u);
  ASSERT_EQ(page.items.size(), 3u);
  EXPECT_EQ(page.items[0].seriesName, "星光");
  EXPECT_EQ(page.items[0].seriesIndex, "3");
  EXPECT_EQ(page.items[1].seriesIndex, "1b");
  EXPECT_TRUE(page.items[2].seriesName.empty());
  EXPECT_TRUE(page.items[2].seriesIndex.empty());
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesUsesOneBasedWirePagesForFirstAndLaterPages) {
  FakeKOSyncTransport* fake = nullptr;
  auto session =
      makeJsonSession({{200, R"({"hasNext":true,"items":[]})"}, {200, R"({"hasNext":false,"items":[]})"}}, &fake);
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("authors", 0, page), Client::OK);
  EXPECT_TRUE(page.hasNext);
  ASSERT_EQ(session->fetchSectionEntries("authors", 3, page), Client::OK);
  EXPECT_FALSE(page.hasNext);

  ASSERT_EQ(fake->urls.size(), 2u);
  EXPECT_NE(fake->urls[0].find("?page=1&size=20"), std::string::npos) << fake->urls[0];
  EXPECT_NE(fake->urls[1].find("?page=4&size=20"), std::string::npos) << fake->urls[1];
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesRejectsEmptySectionId) {
  auto session = makeJsonSession({});
  Client::FacetPage page;
  EXPECT_EQ(session->fetchSectionEntries("", 0, page), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesEncodesSectionIdInPath) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"items":[]})"}}, &fake);
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("weird/id", 0, page), Client::OK);
  ASSERT_EQ(fake->urls.size(), 1u);
  // A '/' inside the section id must not be able to introduce an extra path segment.
  EXPECT_NE(fake->urls[0].find("sections/weird%2Fid"), std::string::npos) << fake->urls[0];
}

TEST_F(BookOrbitCatalogClientTest, FetchSectionEntriesParsesLibrariesCollectionsAndSmartScopes) {
  // Libraries/collections go through the exact same parsing code path as authors/series -- these
  // three cover the "count" field name (verified against the official server's own
  // getLibraryEntries/getCollectionEntries) and the SmartScope shape, which omits "count" entirely
  // (entry["count"] | 0 must still default cleanly rather than reject the entry).
  for (const char* sectionId : {"libraries", "collections", "smart-scopes"}) {
    FakeKOSyncTransport* fake = nullptr;
    auto session = makeJsonSession(
        {{200, sectionId == std::string("smart-scopes") ? R"({"items":[{"id":"3","title":"Night Reads"}]})"
                                                        : R"({"items":[{"id":"3","title":"Sci-Fi","count":12}]})"}},
        &fake);
    Client::FacetPage page;
    ASSERT_EQ(session->fetchSectionEntries(sectionId, 0, page), Client::OK) << sectionId;
    ASSERT_EQ(page.items.size(), 1u) << sectionId;
    EXPECT_EQ(page.items[0].id, "3") << sectionId;
    EXPECT_EQ(page.items[0].title, sectionId == std::string("smart-scopes") ? "Night Reads" : "Sci-Fi") << sectionId;
    if (sectionId != std::string("smart-scopes")) EXPECT_EQ(page.items[0].bookCount, 12u) << sectionId;
    ASSERT_EQ(fake->urls.size(), 1u);
    EXPECT_NE(fake->urls[0].find(std::string("sections/") + sectionId), std::string::npos) << fake->urls[0];
  }
}

// ---- real-server id/field shapes (packages/types/src/koreader.ts @ f2a7a11) ---------------------

TEST_F(BookOrbitCatalogClientTest, FetchBooksAcceptsTheServersNumericIdsAndListExtras) {
  auto session = makeJsonSession({{200, R"({"page":1,"total":2,"size":20,"hasNext":false,"items":[
      {"id":101,"title":"沙丘","authors":["法蘭克·赫伯特"],"progressPercentage":42.5,"readStatus":"reading"},
      {"id":"b2","title":"String id still works"},
      {"id":null,"title":"null id is skipped"},
      {"id":1.5,"title":"float id is skipped"}]})"}});
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks({}, page), Client::OK);
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].id, "101");
  EXPECT_FLOAT_EQ(page.items[0].progressPercent, 42.5f);
  EXPECT_EQ(page.items[0].readStatus, "reading");
  EXPECT_EQ(page.items[1].id, "b2");
  EXPECT_LT(page.items[1].progressPercent, 0.0f) << "absent progress stays unknown";
}

TEST_F(BookOrbitCatalogClientTest, AnOverLongStringIdIsRejectedNotTruncated) {
  const std::string longId(200, '7');
  auto session = makeJsonSession({{200, R"({"items":[{"id":")" + longId + R"(","title":"x"}]})"}});
  Client::BookPage page;
  ASSERT_EQ(session->fetchBooks({}, page), Client::OK);
  EXPECT_TRUE(page.items.empty()) << "a truncated id could name a different book";
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailSurvivesAServerSizedDescriptionBeforeFiles) {
  // The server emits description (often several KB) and relatedSections around `files`; the
  // retained body must still hold the files after streaming in socket-sized pieces.
  std::string description;
  for (int i = 0; i < 300; i++) description += "夜色漸深，圖書館的燈一盞一盞亮起。";
  std::string related = R"([{"id":"author","title":"More","books":[)";
  for (int i = 0; i < 8; i++) {
    related += std::string(i ? "," : "") + R"({"id":)" + std::to_string(900 + i) +
               R"(,"title":"Related Book With A Long Title","authors":["Someone"],"thumbnailUrl":"/api/v1/koreader/plugin/catalog/books/900/thumbnail"})";
  }
  related += "]}]";
  const std::string body = R"({"id":7,"title":"長夜將盡的星光圖書館","authors":["林曉雲"],"seriesId":3,)"
                           R"("seriesName":"星光圖書館三部曲","seriesIndex":2.5,"progressPercentage":45.2,)"
                           R"("readStatus":"reading","description":")" +
                           description + R"(","rating":4,"files":[{"id":70,"format":"epub","sizeBytes":1234,)"
                                         R"("devicePath":"/books/長夜將盡的星光圖書館.epub"}],"relatedSections":)" +
                           related + "}";
  ASSERT_GT(body.size(), 12000u);
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, body}}, &fake);
  fake->chunkSize = 517;
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("7", "device-1", detail), Client::OK);
  EXPECT_EQ(detail.title, "長夜將盡的星光圖書館");
  ASSERT_EQ(detail.files.size(), 1u);
  EXPECT_EQ(detail.files[0].id, "70");
  EXPECT_EQ(detail.rating, 4);
  EXPECT_EQ(detail.seriesName, "星光圖書館三部曲");
  EXPECT_EQ(detail.seriesIndex, "2.5");
  EXPECT_NEAR(detail.progressPercent, 45.2f, 0.01f);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailParsesNumericIdsAndOnlyAWellFormedFileHash) {
  auto session = makeJsonSession({{200, R"({"id":7,"title":"Dune","authors":["Frank Herbert"],
      "files":[{"id":70,"format":"epub","sizeBytes":200,"fileHash":"0123456789abcdef0123456789abcdef"},
               {"id":71,"format":"EPUB","sizeBytes":300,"fileHash":"0123456789ABCDEF0123456789ABCDEF"},
               {"id":72,"format":"epub","sizeBytes":400,"fileHash":null}]})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("7", "dev1", detail), Client::OK);
  EXPECT_EQ(detail.id, "7");
  ASSERT_EQ(detail.files.size(), 3u);
  EXPECT_EQ(detail.files[0].id, "70");
  EXPECT_EQ(detail.files[0].fileHash, "0123456789abcdef0123456789abcdef");
  EXPECT_TRUE(detail.files[1].fileHash.empty()) << "not the documented lowercase partial-MD5 shape";
  EXPECT_TRUE(detail.files[2].fileHash.empty());
}

// ---- fetchBookDetail --------------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailFiltersToEpubOnlyCaseInsensitively) {
  auto session = makeJsonSession({{200,
                                   R"({"id":"b1","title":"Dune","authors":["Frank Herbert"],
            "files":[{"id":"f1","format":"PDF","sizeBytes":100},
                     {"id":"f2","format":"EPUB","sizeBytes":200}]})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  ASSERT_EQ(detail.files.size(), 1u);
  EXPECT_EQ(detail.files[0].id, "f2");
  EXPECT_EQ(detail.files[0].format, "epub");
  EXPECT_EQ(detail.files[0].sizeBytes, 200u);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailEmptyFileListWhenNoCompatibleFormat) {
  auto session =
      makeJsonSession({{200, R"({"id":"b1","title":"Dune","files":[{"id":"f1","format":"pdf","sizeBytes":100}]})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  EXPECT_TRUE(detail.files.empty());
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailSendsTheGivenDeviceIdQueryParam) {
  // Deliberately a device-specific test value, not the shared BookOrbitLimits::DEVICE_ID literal:
  // this method's contract is to send whatever unique per-installation id the caller passes
  // (see its own doc comment on why), not a single literal every X3 device would share.
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune"})"}}, &fake);
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "a1b2c3d4e5f60718", detail), Client::OK);
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_NE(fake->urls[0].find("deviceId=a1b2c3d4e5f60718"), std::string::npos) << fake->urls[0];
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailRejectsAnEmptyInstallationId) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune"})"}}, &fake);
  Client::BookDetail detail;
  EXPECT_EQ(session->fetchBookDetail("b1", "", detail), Client::BAD_REQUEST);
  EXPECT_TRUE(fake->urls.empty());
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailRejectsMissingRequiredFields) {
  auto session = makeJsonSession({{200, R"({"title":"No id"})"}});
  Client::BookDetail detail;
  EXPECT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailRejectsEmptyId) {
  auto session = makeJsonSession({});
  Client::BookDetail detail;
  EXPECT_EQ(session->fetchBookDetail("", "dev1", detail), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailParsesReadStatusAndRating) {
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune","readStatus":"reading","rating":4})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  EXPECT_EQ(detail.readStatus, "reading");
  EXPECT_EQ(detail.rating, 4);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailNullRatingIsMinusOneNotZero) {
  // Regression: `doc["rating"] | -1` would misparse an explicit 0 as the fallback, since
  // ArduinoJson's `|` only substitutes on a type mismatch, not on 0 specifically. This distinguishes
  // 0 (a legitimate wire value) from null/absent (unrated) via isNull(), never `|`.
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune","rating":null})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  EXPECT_EQ(detail.rating, -1);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailZeroRatingIsPreservedNotTreatedAsUnrated) {
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune","rating":0})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  EXPECT_EQ(detail.rating, 0);
}

TEST_F(BookOrbitCatalogClientTest, FetchBookDetailMissingReadStatusAndRatingDefaultToUnset) {
  auto session = makeJsonSession({{200, R"({"id":"b1","title":"Dune"})"}});
  Client::BookDetail detail;
  ASSERT_EQ(session->fetchBookDetail("b1", "dev1", detail), Client::OK);
  EXPECT_TRUE(detail.readStatus.empty());
  EXPECT_EQ(detail.rating, -1);
}

// ---- setReadStatus ------------------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, SetReadStatusSendsPutWithStatusBodyAndParsesEcho) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"readStatus":"want_to_read"})"}}, &fake);
  std::string outStatus;
  ASSERT_EQ(session->setReadStatus("b1", "want_to_read", outStatus), Client::OK);
  EXPECT_EQ(outStatus, "want_to_read");
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_EQ(fake->requests[0].method, "PUT");
  EXPECT_NE(fake->requests[0].url.find("books/b1/read-status"), std::string::npos) << fake->requests[0].url;
  EXPECT_NE(fake->requests[0].payload.find(R"("status":"want_to_read")"), std::string::npos)
      << fake->requests[0].payload;
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusAcceptsEveryDocumentedValue) {
  for (size_t i = 0; i < Client::SETTABLE_READ_STATUS_COUNT; i++) {
    const std::string status = Client::SETTABLE_READ_STATUSES[i];
    auto session = makeJsonSession({{200, R"({"readStatus":")" + status + R"("})"}});
    std::string outStatus;
    ASSERT_EQ(session->setReadStatus("b1", status, outStatus), Client::OK) << status;
    EXPECT_EQ(outStatus, status);
  }
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusRejectsUnknownValueWithoutSendingARequest) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({}, &fake);
  std::string outStatus;
  // "complete" is the bulk-upload vocabulary's spelling, not this route's ("read") -- exactly the
  // cross-vocabulary mistake this client-side check exists to catch before spending a round-trip.
  EXPECT_EQ(session->setReadStatus("b1", "complete", outStatus), Client::BAD_REQUEST);
  EXPECT_TRUE(fake->requests.empty());
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusRejectsEmptyStatus) {
  auto session = makeJsonSession({});
  std::string outStatus;
  EXPECT_EQ(session->setReadStatus("b1", "", outStatus), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusRejectsEmptyBookId) {
  auto session = makeJsonSession({});
  std::string outStatus;
  EXPECT_EQ(session->setReadStatus("", "reading", outStatus), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusRejectsMalformedResponse) {
  auto session = makeJsonSession({{200, R"({"unexpected":true})"}});
  std::string outStatus;
  EXPECT_EQ(session->setReadStatus("b1", "reading", outStatus), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusAuthFailureMapsTo401) {
  auto session = makeJsonSession({{401, ""}});
  std::string outStatus;
  EXPECT_EQ(session->setReadStatus("b1", "reading", outStatus), Client::AUTH_FAILED);
}

TEST_F(BookOrbitCatalogClientTest, SetReadStatusNoCredentialsShortCircuits) {
  BOOKORBIT_STORE.clearCredentials();
  auto session = makeJsonSession({});
  std::string outStatus;
  EXPECT_EQ(session->setReadStatus("b1", "reading", outStatus), Client::NO_CREDENTIALS);
}

// ---- setRating ------------------------------------------------------------------------------

TEST_F(BookOrbitCatalogClientTest, SetRatingSendsPutWithIntegerBodyAndParsesEcho) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"rating":4})"}}, &fake);
  int outRating = -1;
  ASSERT_EQ(session->setRating("b1", 4, outRating), Client::OK);
  EXPECT_EQ(outRating, 4);
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_EQ(fake->requests[0].method, "PUT");
  EXPECT_NE(fake->requests[0].url.find("books/b1/rating"), std::string::npos) << fake->requests[0].url;
  EXPECT_NE(fake->requests[0].payload.find(R"("rating":4)"), std::string::npos) << fake->requests[0].payload;
}

TEST_F(BookOrbitCatalogClientTest, SetRatingMinusOneSendsJsonNullToClear) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"rating":null})"}}, &fake);
  int outRating = 99;
  ASSERT_EQ(session->setRating("b1", -1, outRating), Client::OK);
  EXPECT_EQ(outRating, -1);
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_NE(fake->requests[0].payload.find(R"("rating":null)"), std::string::npos) << fake->requests[0].payload;
}

TEST_F(BookOrbitCatalogClientTest, SetRatingAcceptsZero) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({{200, R"({"rating":0})"}}, &fake);
  int outRating = -1;
  ASSERT_EQ(session->setRating("b1", 0, outRating), Client::OK);
  EXPECT_EQ(outRating, 0);
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_NE(fake->requests[0].payload.find(R"("rating":0)"), std::string::npos) << fake->requests[0].payload;
}

TEST_F(BookOrbitCatalogClientTest, SetRatingRejectsOutOfRangeWithoutSendingARequest) {
  FakeKOSyncTransport* fake = nullptr;
  auto session = makeJsonSession({}, &fake);
  int outRating = -1;
  EXPECT_EQ(session->setRating("b1", 6, outRating), Client::BAD_REQUEST);
  EXPECT_EQ(session->setRating("b1", -2, outRating), Client::BAD_REQUEST);
  EXPECT_TRUE(fake->requests.empty());
}

TEST_F(BookOrbitCatalogClientTest, SetRatingRejectsEmptyBookId) {
  auto session = makeJsonSession({});
  int outRating = -1;
  EXPECT_EQ(session->setRating("", 3, outRating), Client::BAD_REQUEST);
}

TEST_F(BookOrbitCatalogClientTest, SetRatingRejectsMalformedResponse) {
  auto session = makeJsonSession({{200, "not json"}});
  int outRating = -1;
  EXPECT_EQ(session->setRating("b1", 3, outRating), Client::JSON_ERROR);
}

TEST_F(BookOrbitCatalogClientTest, SetRatingNoCredentialsShortCircuits) {
  BOOKORBIT_STORE.clearCredentials();
  auto session = makeJsonSession({});
  int outRating = -1;
  EXPECT_EQ(session->setRating("b1", 3, outRating), Client::NO_CREDENTIALS);
}

// ---- downloadFile: SD-side atomicity, via an injected FileFetcher -----------------------------

// Simulates a successful network fetch by writing `content` straight into the in-memory
// filesystem at tmpPath, exactly as HttpDownloader::downloadToFile() would have left it on a real
// device. Tracks invocation so a test can assert the fetcher was (or was not) even attempted.
struct ScriptedFetch {
  int httpCode = 200;
  bool localFileError = false;
  bool aborted = false;
  std::string content = "epub-bytes";
  int calls = 0;

  Client::FetchOutcome operator()(const std::string&, const std::string& tmpPath, const Client::ProgressCallback&,
                                  bool*) {
    calls++;
    Client::FetchOutcome outcome;
    outcome.httpCode = httpCode;
    outcome.localFileError = localFileError;
    outcome.aborted = aborted;
    if (httpCode >= 200 && httpCode < 300 && !localFileError && !aborted) {
      HalFile file;
      Storage.openFileForWrite("TEST", tmpPath, file);
      file.write(reinterpret_cast<const uint8_t*>(content.data()), content.size());
      file.close();
    }
    return outcome;
  }
};

std::unique_ptr<Client::Session> makeDownloadSession(ScriptedFetch& fetch) {
  auto* fake = new FakeKOSyncTransport({});
  return std::make_unique<Client::Session>(std::unique_ptr<KOSyncTransport>(fake), std::ref(fetch));
}

std::string fileContent(const std::string& path) {
  auto it = testStorage().files.find(path);
  return it == testStorage().files.end() ? "" : std::string(it->second.begin(), it->second.end());
}

std::string downloadTransactionPath(const std::string& destPath) { return destPath + ".bookorbit-download.txn"; }

void seedDownloadTransaction(const std::string& destPath) {
  const std::string contents = "CrossFade BookOrbit catalog download transaction v1\n" + destPath;
  testStorage().files[downloadTransactionPath(destPath)] = std::vector<uint8_t>(contents.begin(), contents.end());
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileSucceedsAndPromotesTempIntoPlace) {
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::OK);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "epub-bytes");
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));
  EXPECT_FALSE(Storage.exists(downloadTransactionPath("/Books/dune.epub").c_str()));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileDoesNotFetchUntilItsOwnershipMarkerIsDurable) {
  testStorage().failFlush = true;
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileReplacesExistingFileViaBackupThenCleansUp) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  fetch.content = "new-bytes";
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::OK);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "new-bytes");
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileShortWriteReportedAsFileErrorLeavesDestUntouched) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  fetch.localFileError = true;  // models a failed close()/flush after bytes were written
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "old");  // original file never touched
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileAbortedLeavesDestUntouchedAndCleansTemp) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  fetch.aborted = true;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::ABORTED);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "old");
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileNonSuccessHttpStatusLeavesDestUntouched) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  fetch.httpCode = 404;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::UNSUPPORTED);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "old");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileAuthFailureMapsTo401) {
  ScriptedFetch fetch;
  fetch.httpCode = 401;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::AUTH_FAILED);
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileRenameOfExistingFileAsideFailureRestoresNothingButKeepsOriginal) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  testStorage().failRenameAt = 1;  // fails the dest -> .bak rename specifically
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "old");  // original file is exactly where it was
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePromoteRenameFailureRestoresBackup) {
  testStorage().files["/Books/dune.epub"] = {'o', 'l', 'd'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  testStorage().failRenameAt = 2;  // fails the .tmp -> dest promotion after staging succeeded
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "old");  // restored from .bak
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
}

TEST_F(BookOrbitCatalogClientTest,
       DownloadFileRecoversFromAStaleBackupLeftByAnInterruptedPromotionCleanupWhenDestAlsoExists) {
  // The durable marker proves that these otherwise user-visible sidecars belong to BookOrbit.
  testStorage().files["/Books/dune.epub"] = {'n', 'e', 'w'};
  testStorage().files["/Books/dune.epub.bak"] = {'s', 't', 'a', 'l', 'e'};
  seedDownloadTransaction("/Books/dune.epub");
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::OK);
  EXPECT_EQ(fetch.calls, 1);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "epub-bytes");  // the freshly fetched content
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));      // stale backup cleared, not restored
  EXPECT_FALSE(Storage.exists(downloadTransactionPath("/Books/dune.epub").c_str()));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePreservesAnUnownedBackupBesideAnExistingDestination) {
  testStorage().files["/Books/dune.epub"] = {'b', 'o', 'o', 'k'};
  testStorage().files["/Books/dune.epub.bak"] = {'u', 's', 'e', 'r'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "book");
  EXPECT_EQ(fileContent("/Books/dune.epub.bak"), "user");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileRefusesWhenAStaleBackupCannotBeCleared) {
  // The backup-clear itself failing is a real device I/O problem, not an ambiguous state --
  // refuse rather than risk an overwrite-via-rename against a backup that did not actually clear.
  testStorage().files["/Books/dune.epub"] = {'n', 'e', 'w'};
  testStorage().files["/Books/dune.epub.bak"] = {'s', 't', 'a', 'l', 'e'};
  seedDownloadTransaction("/Books/dune.epub");
  testStorage().failRemoves = true;
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);                                // never even attempted the network fetch
  EXPECT_EQ(fileContent("/Books/dune.epub"), "new");        // untouched
  EXPECT_EQ(fileContent("/Books/dune.epub.bak"), "stale");  // untouched -- the clear itself failed
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileRecoversAnOriginalHiddenByAResetBetweenTheTwoPromoteRenames) {
  // The marker plus hidden backup identifies an interrupted BookOrbit promotion.
  testStorage().files["/Books/dune.epub.bak"] = {'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  seedDownloadTransaction("/Books/dune.epub");
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "original") << "the hidden original must become visible again";
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak")) << "must not still look like an unresolved backup afterward";
  EXPECT_FALSE(Storage.exists(downloadTransactionPath("/Books/dune.epub").c_str()));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePreservesAnUnownedBackupWhenDestinationIsAbsent) {
  testStorage().files["/Books/dune.epub.bak"] = {'u', 's', 'e', 'r'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_FALSE(Storage.exists("/Books/dune.epub"));
  EXPECT_EQ(fileContent("/Books/dune.epub.bak"), "user");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePreservesHiddenBackupWhenDestinationAbsenceCannotBeProved) {
  // SdFat's exists() is implemented as an attempted open, so false can mean either absent or a
  // transient directory-read failure. An error probing the destination must not make the hidden
  // backup look like the unambiguous reset-between-renames state, nor may a later probe delete it.
  testStorage().files["/Books/dune.epub.bak"] = {'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  testStorage().failPathProbes.insert("/Books/dune.epub");
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_FALSE(Storage.exists("/Books/dune.epub"));
  EXPECT_EQ(fileContent("/Books/dune.epub.bak"), "original") << "the only complete copy must stay recoverable";
}

TEST_F(BookOrbitCatalogClientTest, ARetryAfterRecoveringAHiddenOriginalSucceedsNormally) {
  // Restart test: the recovery above must not leave the download permanently wedged -- the very
  // next attempt (this is what an automatic retry or the user pressing download again looks like)
  // must proceed as an entirely normal download once the anomaly is reconciled.
  testStorage().files["/Books/dune.epub.bak"] = {'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  seedDownloadTransaction("/Books/dune.epub");
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  ASSERT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);  // first call: recovers only

  ASSERT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::OK);  // second call: a normal download
  EXPECT_EQ(fetch.calls, 1);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "epub-bytes");
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.bak"));
  EXPECT_FALSE(Storage.exists("/Books/dune.epub.tmp"));
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileRecoveryRestoresBackupWhenNoOriginalExistedBeforeTheFirstDownload) {
  // Same reset-between-renames signature, but for a book downloaded for the very first time (no
  // pre-existing dest before that attempt) -- .bak in that case holds nothing (this scenario is
  // only reachable via the ambiguous ".bak but no prior dest" state some other way; included to
  // pin the recovery rename's own failure path). If the recovery rename itself fails (e.g. a
  // second, unrelated storage fault), the call must still refuse rather than silently proceeding.
  testStorage().files["/Books/dune.epub.bak"] = {'o', 'r', 'i', 'g'};
  seedDownloadTransaction("/Books/dune.epub");
  testStorage().failRenames = true;
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_TRUE(Storage.exists("/Books/dune.epub.bak")) << "recovery rename failed; backup is left in place, not lost";
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileDiscardsLeftoverTempFromAnEarlierInterruptedAttempt) {
  testStorage().files["/Books/dune.epub.tmp"] = {'g', 'a', 'r', 'b', 'a', 'g', 'e'};
  seedDownloadTransaction("/Books/dune.epub");
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::OK);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "epub-bytes");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePreservesAnUnownedTempFile) {
  testStorage().files["/Books/dune.epub.tmp"] = {'u', 's', 'e', 'r'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_EQ(fileContent("/Books/dune.epub.tmp"), "user");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFilePreservesSidecarsWhenTheMarkerDoesNotMatch) {
  testStorage().files["/Books/dune.epub"] = {'b', 'o', 'o', 'k'};
  testStorage().files["/Books/dune.epub.bak"] = {'u', 's', 'e', 'r', '-', 'b', 'a', 'k'};
  testStorage().files["/Books/dune.epub.tmp"] = {'u', 's', 'e', 'r', '-', 't', 'm', 'p'};
  testStorage().files[downloadTransactionPath("/Books/dune.epub")] = {'n', 'o', 't', '-', 'o', 'u', 'r', 's'};
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);

  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
  EXPECT_EQ(fetch.calls, 0);
  EXPECT_EQ(fileContent("/Books/dune.epub"), "book");
  EXPECT_EQ(fileContent("/Books/dune.epub.bak"), "user-bak");
  EXPECT_EQ(fileContent("/Books/dune.epub.tmp"), "user-tmp");
  EXPECT_EQ(fileContent(downloadTransactionPath("/Books/dune.epub")), "not-ours");
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileRejectsEmptyFileId) {
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("", "/Books/dune.epub"), Client::BAD_REQUEST);
  EXPECT_EQ(fetch.calls, 0);
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileNoCredentialsShortCircuits) {
  BOOKORBIT_STORE.setTestCredentials(false);
  ScriptedFetch fetch;
  auto session = makeDownloadSession(fetch);
  EXPECT_EQ(session->downloadFile("f1", "/Books/dune.epub"), Client::NO_CREDENTIALS);
  EXPECT_EQ(fetch.calls, 0);
}

TEST_F(BookOrbitCatalogClientTest, DownloadFileWithoutAFileFetcherFailsClosedRatherThanCrash) {
  // The 1-arg (transport-only) constructor deliberately leaves fileFetcher_ unset -- see
  // BookOrbitCatalogClient.cpp's comment on that constructor. Calling downloadFile() on a Session
  // built that way must fail closed, not call an empty std::function.
  auto* fake = new FakeKOSyncTransport({});
  Client::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  EXPECT_EQ(session.downloadFile("f1", "/Books/dune.epub"), Client::FILE_ERROR);
}

}  // namespace

// ---- Heap preparation before the TLS gate --------------------------------------------------------
// The browser frees the SD-font page arena it just rendered with right before each request's heap
// gate. These run every catalog operation with the fake heap BELOW KOSyncRequest's gate
// (35000 free / 20000 largest block) and a hook that frees memory, so a hook that ran after the
// gate (or not at all) would surface as LOW_MEMORY.
namespace {
struct HeapPressure {
  uint32_t freeAfter = 60000;
  uint32_t blockAfter = 30000;
  int calls = 0;
  size_t requestsAtCall = 0;  // transport requests already sent when the hook ran
  FakeKOSyncTransport* fake = nullptr;
  static void release(void* context) {
    auto* self = static_cast<HeapPressure*>(context);
    self->calls++;
    self->requestsAtCall = self->fake ? self->fake->requests.size() : 0;
    ESP.freeHeap = self->freeAfter;
    ESP.maxAllocHeap = self->blockAfter;
  }
};

void squeezeHeap() {
  ESP.freeHeap = 30000;       // < MIN_FREE_FOR_TLS
  ESP.maxAllocHeap = 19000;   // < MIN_BLOCK_FOR_TLS
}

// One call of each operation against a session holding exactly one scripted success response.
enum class Op { Root, Books, Facets, Detail, ReadStatus, Rating, Download };
const Op ALL_OPS[] = {Op::Root, Op::Books, Op::Facets, Op::Detail, Op::ReadStatus, Op::Rating, Op::Download};

const char* opName(Op op) {
  switch (op) {
    case Op::Root: return "root";
    case Op::Books: return "books";
    case Op::Facets: return "facets";
    case Op::Detail: return "detail";
    case Op::ReadStatus: return "read-status";
    case Op::Rating: return "rating";
    case Op::Download: return "download";
  }
  return "?";
}

FakeKOSyncTransport::ScriptedResponse responseFor(Op op) {
  switch (op) {
    case Op::Root: return {200, R"({"sections":[{"id":"recent","title":"Recent"}]})"};
    case Op::Books: return {200, R"({"items":[{"id":"b1","title":"Dune"}],"hasNext":false})"};
    case Op::Facets: return {200, R"({"hasNext":false,"items":[{"id":"a1","title":"Herbert","count":1}]})"};
    case Op::Detail: return {200, R"({"id":"7","title":"Dune","files":[]})"};
    case Op::ReadStatus: return {200, R"({"readStatus":"reading"})"};
    case Op::Rating: return {200, R"({"rating":3})"};
    case Op::Download: return {200, ""};
  }
  return {500, ""};
}

Client::Error runOp(Op op, Client::Session& session) {
  switch (op) {
    case Op::Root: { std::vector<Client::Section> out; return session.fetchRootSections(out); }
    case Op::Books: { Client::BookQuery q; Client::BookPage out; return session.fetchBooks(q, out); }
    case Op::Facets: { Client::FacetPage out; return session.fetchSectionEntries("authors", 0, out); }
    case Op::Detail: { Client::BookDetail out; return session.fetchBookDetail("7", "dev1", out); }
    case Op::ReadStatus: { std::string out; return session.setReadStatus("b1", "reading", out); }
    case Op::Rating: { int out = -1; return session.setRating("b1", 3, out); }
    case Op::Download: return session.downloadFile("f1", "/Books/dune.epub");
  }
  return Client::BAD_REQUEST;
}

std::unique_ptr<Client::Session> makeOpSession(Op op, ScriptedFetch& fetch, FakeKOSyncTransport** outFake) {
  auto* fake = new FakeKOSyncTransport({responseFor(op)});
  *outFake = fake;
  if (op == Op::Download) {
    return std::make_unique<Client::Session>(std::unique_ptr<KOSyncTransport>(fake), std::ref(fetch));
  }
  return std::make_unique<Client::Session>(std::unique_ptr<KOSyncTransport>(fake));
}
}  // namespace

TEST_F(BookOrbitCatalogClientTest, EveryOperationReleasesHeapBeforeTheTlsGate) {
  for (const Op op : ALL_OPS) {
    SCOPED_TRACE(opName(op));
    testStorage().reset();
    ScriptedFetch fetch;
    HeapPressure pressure;
    auto session = makeOpSession(op, fetch, &pressure.fake);
    session->setPrepareHeap(&HeapPressure::release, &pressure);
    squeezeHeap();
    EXPECT_EQ(runOp(op, *session), Client::OK);
    EXPECT_EQ(pressure.calls, 1);
    EXPECT_EQ(pressure.requestsAtCall, 0u);  // ran before anything went on the wire
  }
}

TEST_F(BookOrbitCatalogClientTest, WithoutHeapPreparationThePressuredGateStillRefusesEveryOperation) {
  for (const Op op : ALL_OPS) {
    SCOPED_TRACE(opName(op));
    ScriptedFetch fetch;
    FakeKOSyncTransport* fake = nullptr;
    auto session = makeOpSession(op, fetch, &fake);
    squeezeHeap();
    EXPECT_EQ(runOp(op, *session), Client::LOW_MEMORY);
    EXPECT_TRUE(fake->requests.empty());
    EXPECT_EQ(fetch.calls, 0);
  }
}

TEST_F(BookOrbitCatalogClientTest, HeapPreparationThatFreesTooLittleKeepsTheGateClosed) {
  // The hook is a release attempt, not a bypass: if it cannot lift both figures over the gate, the
  // request is still refused before any TLS work -- free heap alone is not enough without a block.
  for (const Op op : ALL_OPS) {
    SCOPED_TRACE(opName(op));
    ScriptedFetch fetch;
    HeapPressure pressure;
    pressure.freeAfter = 80000;
    pressure.blockAfter = 19999;
    auto session = makeOpSession(op, fetch, &pressure.fake);
    session->setPrepareHeap(&HeapPressure::release, &pressure);
    squeezeHeap();
    EXPECT_EQ(runOp(op, *session), Client::LOW_MEMORY);
    EXPECT_EQ(pressure.calls, 1);
    EXPECT_TRUE(pressure.fake->requests.empty());
    EXPECT_EQ(fetch.calls, 0);
  }
}

TEST_F(BookOrbitCatalogClientTest, HeapPreparationRunsAgainForEveryRequestOnAReusedSession) {
  // The browser keeps one keep-alive session across screens, and every status render in between can
  // rebuild a page arena: each request must release again, not only the first.
  HeapPressure pressure;
  auto session = makeJsonSession({responseFor(Op::Root), responseFor(Op::Books), responseFor(Op::Root)},
                                 &pressure.fake);
  session->setPrepareHeap(&HeapPressure::release, &pressure);
  for (int round = 0; round < 3; ++round) {
    squeezeHeap();  // the previous screen's render
    const Op op = round == 1 ? Op::Books : Op::Root;
    ScriptedFetch unused;
    EXPECT_EQ(runOp(op, *session), Client::OK) << round;
    EXPECT_EQ(pressure.calls, round + 1);
    EXPECT_EQ(pressure.requestsAtCall, static_cast<size_t>(round));
  }
}

TEST_F(BookOrbitCatalogClientTest, RequestsRejectedBeforeTheGateDoNotRunHeapPreparation) {
  HeapPressure pressure;
  auto session = makeJsonSession({}, &pressure.fake);
  session->setPrepareHeap(&HeapPressure::release, &pressure);
  std::string out;
  EXPECT_EQ(session->setReadStatus("b1", "not-a-status", out), Client::BAD_REQUEST);
  BOOKORBIT_STORE.setTestCredentials(false);
  std::vector<Client::Section> sections;
  EXPECT_EQ(session->fetchRootSections(sections), Client::NO_CREDENTIALS);
  EXPECT_EQ(pressure.calls, 0);
}

TEST_F(BookOrbitCatalogClientTest, FacetCountsDistinguishZeroFromUnreported) {
  auto session = makeJsonSession({{200, R"({"items":[{"id":1,"title":"No count"},{"id":2,"title":"Empty","count":0}],"hasNext":false})"}});
  Client::FacetPage page;
  ASSERT_EQ(session->fetchSectionEntries("collections", 0, page), Client::OK);
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_FALSE(page.items[0].hasBookCount);
  EXPECT_TRUE(page.items[1].hasBookCount);
  EXPECT_EQ(page.items[1].bookCount, 0u);
}
