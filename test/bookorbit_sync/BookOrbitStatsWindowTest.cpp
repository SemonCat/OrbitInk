// Stats drain WINDOW tests over the REAL BookOrbitStatsSync.cpp + client + queue (same target and
// host bindings as BookOrbitSyncPolicyTest.cpp): one sync window drains the current book, then a
// bounded, fairly rotated set of OTHER books, under one deadline that reaches the transport.
//
// What these pin down (docs/bookorbit-home-library.md §5/§9):
//  - a Stopped current-book drain (network/auth/gating) ends the window: no other book is tried;
//  - the deadline is the abort every request receives, so it cuts a request off mid-flight;
//  - rotation is fair: empty/foreign/stale queue files early in directory order never starve a
//    pending queue after them, across windows and across a reboot (the cursor is on SD);
//  - the rotation cursor is owner-scoped and advisory (corrupt => restart, never a failure).
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Arduino.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitStatsQueue.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HalClock.h"
#include "HalStorage.h"
#include "KOSyncTransport.h"
#include "util/BookOrbitStatsSync.h"
#include "util/StatsDrainCursor.h"

extern HalClock halClock;

namespace {

constexpr uint32_t NOW_MINUTES = 29372160u;
constexpr uint32_t EVENT_START = 1762329600u;  // inside the 30-day window ending NOW_MINUTES*60

// Responds per request by URL, recording which document each page-stats POST carried and calling
// an optional hook mid-request (to model time passing inside a request).
class ScriptedTransport final : public KOSyncTransport {
 public:
  struct Request {
    std::string url;
    std::string payload;
  };
  int pageStatsCode = 200;
  bool acceptAll = true;  // answer page-stats with an "accepted" body for whichever hash was sent
  uint32_t advanceMsPerRequest = 0;
  std::vector<Request> requests;

  bool begin(const std::string& url) override {
    url_ = url;
    return true;
  }
  void addHeader(const std::string&, const std::string&) override {}
  int sendRequest(const char*, const uint8_t* payload, size_t len, const DataCallback& onData,
                  const AbortCallback& shouldAbort) override {
    requests.push_back({url_, payload ? std::string(reinterpret_cast<const char*>(payload), len) : ""});
    hostMillisNow += advanceMsPerRequest;
    // A real SecureHttpClient polls shouldAbort while waiting for headers/body.
    if (shouldAbort && shouldAbort()) return -1;
    if (url_.find("/plugin/version") != std::string::npos) {
      body_ = R"({"pluginVersion":"0.5.1","serverVersion":"1.2.3","capabilities":[]})";
    } else if (url_.find("/plugin/page-stats") != std::string::npos) {
      if (pageStatsCode != 200) return pageStatsCode;
      const std::string hash = hashIn(requests.back().payload);
      body_ = R"({"results":[{"hash":")" + hash + R"(","accepted":1,"duplicates":0}],"unmatched":[]})";
    } else {
      return 404;
    }
    if (onData) onData(reinterpret_cast<const uint8_t*>(body_.data()), body_.size());
    return 200;
  }
  const std::string& getString() const override { return body_; }
  void end() override {}

  std::vector<std::string> statsHashes() const {
    std::vector<std::string> out;
    for (const auto& r : requests) {
      if (r.url.find("/plugin/page-stats") != std::string::npos) out.push_back(hashIn(r.payload));
    }
    return out;
  }

 private:
  static std::string hashIn(const std::string& payload) {
    const size_t at = payload.find("\"hash\":\"");
    return at == std::string::npos ? std::string() : payload.substr(at + 8, 32);
  }
  std::string url_;
  std::string body_;
};

std::string hexId(const unsigned n) {
  char buf[33];
  snprintf(buf, sizeof(buf), "%032x", n);
  return buf;
}

void seed(const std::string& hash, const size_t count = 1) {
  std::vector<BookOrbitStatEvent> events(count);
  for (size_t i = 0; i < count; i++) {
    events[i].startTime = EVENT_START + static_cast<uint32_t>(i) * 60u;
    events[i].durationSeconds = 30;
    events[i].page = 100;
    events[i].totalPages = BookOrbitStatsQueue::PROGRESS_SCALE;
  }
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(hash, BookOrbitStats::currentOwner(), events.data(), count));
}

class StatsWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testStorage().reset();
    BOOKORBIT_STORE.resetToDefaults();
    BOOKORBIT_STORE.setTestBaseUrl("https://books.example.test/api/v1/koreader");
    SETTINGS.resetToDefaults();
    APP_STATE.resetToDefaults();
    halClock.available = true;
    halClock.epochMinutes = NOW_MINUTES;
    hostMillisNow = 1000;
    hostMillisStep = 0;
  }

  BookOrbitStats::WindowResult run(ScriptedTransport*& fake, const std::string& current,
                                   BookOrbitStats::WindowLimits limits = {}) {
    auto owned = std::make_unique<ScriptedTransport>();
    fake = owned.get();
    session_ = std::make_unique<BookOrbitSyncClient::Session>(std::move(owned));
    return BookOrbitStats::drainWindow(*session_, current, limits);
  }

  size_t queued(const std::string& hash) const {
    return BookOrbitStatsQueue::queuedCount(hash, BookOrbitStats::currentOwner());
  }

  std::unique_ptr<BookOrbitSyncClient::Session> session_;
};

// ---- selector (pure) -------------------------------------------------------------------------

TEST(StatsDrainSelectorTest, TakesSmallestIdsAfterTheCursorThenWraps) {
  StatsDrainSelector<3> sel(hexId(5).c_str());
  for (unsigned n : {9u, 2u, 7u, 5u, 6u, 1u}) sel.offer(hexId(n).c_str(), nullptr);
  ASSERT_EQ(sel.count(), 3u);
  EXPECT_EQ(std::string(sel.at(0)), hexId(6));
  EXPECT_EQ(std::string(sel.at(1)), hexId(7));
  EXPECT_EQ(std::string(sel.at(2)), hexId(9));

  StatsDrainSelector<4> wrap(hexId(8).c_str());
  for (unsigned n : {9u, 2u, 7u, 1u}) wrap.offer(hexId(n).c_str(), nullptr);
  ASSERT_EQ(wrap.count(), 4u);
  EXPECT_EQ(std::string(wrap.at(0)), hexId(9));
  EXPECT_EQ(std::string(wrap.at(1)), hexId(1)) << "wrapped ids follow, ascending";
  EXPECT_EQ(std::string(wrap.at(3)), hexId(7));
}

TEST(StatsDrainSelectorTest, ExcludesTheCurrentBookAndDuplicates) {
  StatsDrainSelector<4> sel(nullptr);
  sel.offer(hexId(3).c_str(), hexId(3).c_str());
  sel.offer(hexId(4).c_str(), hexId(3).c_str());
  sel.offer(hexId(4).c_str(), hexId(3).c_str());
  ASSERT_EQ(sel.count(), 1u);
  EXPECT_EQ(std::string(sel.at(0)), hexId(4));
}

// ---- window ----------------------------------------------------------------------------------

TEST_F(StatsWindowTest, DrainsCurrentThenUpToThreeOtherBooksOnOneSession) {
  const std::string current = hexId(1);
  seed(current);
  for (unsigned n = 10; n < 15; n++) seed(hexId(n));
  ScriptedTransport* fake = nullptr;
  const auto r = run(fake, current);
  EXPECT_EQ(r.current, BookOrbitStats::DrainResult::Drained);
  EXPECT_EQ(r.otherAttempted, 3u);
  const auto sent = fake->statsHashes();
  ASSERT_EQ(sent.size(), 4u);
  EXPECT_EQ(sent[0], current) << "current book first";
  EXPECT_EQ(sent[1], hexId(10));
  EXPECT_EQ(sent[3], hexId(12));
  EXPECT_EQ(queued(hexId(13)), 1u) << "beyond the per-window bound waits for the next window";
}

TEST_F(StatsWindowTest, ACurrentBookNetworkFailureEndsTheWholeWindow) {
  const std::string current = hexId(1);
  seed(current);
  seed(hexId(10));
  ScriptedTransport* fake = nullptr;
  auto owned = std::make_unique<ScriptedTransport>();
  owned->pageStatsCode = 503;
  fake = owned.get();
  BookOrbitSyncClient::Session session(std::move(owned));
  const auto r = BookOrbitStats::drainWindow(session, current, {});
  EXPECT_EQ(r.current, BookOrbitStats::DrainResult::Stopped);
  EXPECT_EQ(r.otherAttempted, 0u);
  EXPECT_EQ(fake->statsHashes().size(), 1u) << "the same failure must not be repeated on another book";
  EXPECT_EQ(queued(current), 1u);
  EXPECT_EQ(queued(hexId(10)), 1u);
}

TEST_F(StatsWindowTest, AnOtherBookFailureStopsAndThatBookIsFirstNextWindow) {
  for (unsigned n = 10; n < 13; n++) seed(hexId(n));
  ScriptedTransport* fake = nullptr;
  {
    auto owned = std::make_unique<ScriptedTransport>();
    owned->pageStatsCode = 500;
    fake = owned.get();
    BookOrbitSyncClient::Session session(std::move(owned));
    const auto r = BookOrbitStats::drainWindow(session, "", {});
    EXPECT_TRUE(r.stopped);
    EXPECT_EQ(fake->statsHashes().size(), 1u);
  }
  const auto r2 = run(fake, "");
  EXPECT_FALSE(r2.stopped);
  const auto sent = fake->statsHashes();
  ASSERT_FALSE(sent.empty());
  EXPECT_EQ(sent[0], hexId(10)) << "a Stopped book is not passed over by the rotation";
}

TEST_F(StatsWindowTest, TheDeadlineCutsAnInFlightRequestOffThroughTheTransportAbort) {
  const std::string current = hexId(1);
  seed(current);
  seed(hexId(10));
  ScriptedTransport* fake = nullptr;
  auto owned = std::make_unique<ScriptedTransport>();
  owned->advanceMsPerRequest = 20000;  // one request "takes" longer than the whole budget
  fake = owned.get();
  BookOrbitSyncClient::Session session(std::move(owned));
  BookOrbitStats::WindowLimits limits;
  limits.budgetMs = 15000;
  const auto r = BookOrbitStats::drainWindow(session, current, limits);
  EXPECT_EQ(r.current, BookOrbitStats::DrainResult::Stopped) << "aborted mid-request by the deadline";
  EXPECT_EQ(fake->requests.size(), 1u) << "nothing after the deadline";
  EXPECT_EQ(queued(current), 1u) << "an aborted upload keeps its events";
}

TEST_F(StatsWindowTest, PendingQueuesBehindManyEmptyAndForeignFilesAreReachedFairly) {
  // 40 empty queues and 40 foreign-owner queues sort BEFORE the pending ones, so a prefix scan
  // would never reach them; the rotation must, within a bounded number of windows.
  for (unsigned n = 0; n < 40; n++) {
    seed(hexId(0x100 + n));
    BookOrbitStatsQueue::clear(hexId(0x100 + n));
    testStorage().files[BookOrbitStatsQueue::filePath(hexId(0x100 + n))] = {};  // empty file
  }
  BOOKORBIT_STORE.setTestCredentials(true, "someone-else", "pw", "deadbeefdeadbeefdeadbeefdeadbeef");
  for (unsigned n = 0; n < 40; n++) seed(hexId(0x200 + n));
  BOOKORBIT_STORE.resetToDefaults();
  BOOKORBIT_STORE.setTestBaseUrl("https://books.example.test/api/v1/koreader");
  const std::vector<std::string> pending = {hexId(0x300), hexId(0x301), hexId(0x302), hexId(0x303)};
  for (const auto& h : pending) seed(h);

  std::set<std::string> delivered;
  ScriptedTransport* fake = nullptr;
  int windows = 0;
  // Bound: 84 candidate ids / 8 examined per window => every id is examined within 11 windows, and
  // at most 3 non-empty queues drain per window, so the 4 pending ones need at most one more.
  for (; windows < 12 && delivered.size() < pending.size(); windows++) {
    run(fake, "");
    for (const auto& h : fake->statsHashes()) delivered.insert(h);
  }
  EXPECT_EQ(delivered, std::set<std::string>(pending.begin(), pending.end())) << "after " << windows << " windows";
  for (unsigned n = 0; n < 40; n++) {
    const auto it = testStorage().files.find(BookOrbitStatsQueue::filePath(hexId(0x200 + n)));
    ASSERT_NE(it, testStorage().files.end()) << "a foreign queue is never removed to force fairness";
    EXPECT_GT(it->second.size(), BookOrbitStatsQueue::HEADER_BYTES) << "and never drained";
  }
  for (unsigned n = 0; n < 40; n++) {
    EXPECT_TRUE(testStorage().files.count(BookOrbitStatsQueue::filePath(hexId(0x100 + n))))
        << "nor is an empty one deleted";
  }
}

TEST_F(StatsWindowTest, TheRotationCursorSurvivesAReboot) {
  for (unsigned n = 0; n < 20; n++) seed(hexId(0x10 + n));
  ScriptedTransport* fake = nullptr;
  run(fake, "");
  const auto first = fake->statsHashes();
  ASSERT_EQ(first.size(), 3u);

  // A reboot loses all RAM; only SD state survives. A fresh window continues after the cursor.
  session_.reset();
  run(fake, "");
  const auto second = fake->statsHashes();
  ASSERT_EQ(second.size(), 3u);
  EXPECT_EQ(second[0], hexId(0x13)) << "continues after the last examined id, not from the start";
}

TEST_F(StatsWindowTest, ACorruptOrForeignCursorRestartsTheRotationInsteadOfFailing) {
  for (unsigned n = 0; n < 4; n++) seed(hexId(0x10 + n));
  testStorage().files[BookOrbitStatsQueue::storageDirectory() + "/drain_cursor.dat"] = {1, 2, 3};
  ScriptedTransport* fake = nullptr;
  const auto r = run(fake, "");
  EXPECT_EQ(r.otherAttempted, 3u);
  EXPECT_EQ(fake->statsHashes()[0], hexId(0x10));
}

TEST_F(StatsWindowTest, NoOtherBookIsTriedWhenTheCapabilityProbeFails) {
  seed(hexId(10));
  // Every request fails at the transport: the probe fails and nothing else is attempted.
  struct Failing final : KOSyncTransport {
    int n = 0;
    std::string s;
    bool begin(const std::string&) override { return true; }
    void addHeader(const std::string&, const std::string&) override {}
    int sendRequest(const char*, const uint8_t*, size_t, const DataCallback&, const AbortCallback&) override {
      n++;
      return -1;
    }
    const std::string& getString() const override { return s; }
    void end() override {}
  };
  auto failing = std::make_unique<Failing>();
  Failing* f = failing.get();
  BookOrbitSyncClient::Session session(std::move(failing));
  const auto r = BookOrbitStats::drainWindow(session, hexId(1), {});
  EXPECT_EQ(r.current, BookOrbitStats::DrainResult::Stopped);
  EXPECT_EQ(f->n, 1) << "one probe, then nothing";
  EXPECT_EQ(queued(hexId(10)), 1u);
}

TEST_F(StatsWindowTest, AnUnchangedCursorIsNotRewritten) {
  ScriptedTransport* fake = nullptr;
  seed(hexId(10));
  run(fake, "");
  const std::string path = BookOrbitStatsQueue::storageDirectory() + "/drain_cursor.dat";
  ASSERT_TRUE(testStorage().files.count(path));
  const auto bytes = testStorage().files[path];
  const int renamesBefore = testStorage().renameCount;
  run(fake, "");  // 10 is now empty: examined again, same cursor => no rewrite
  EXPECT_EQ(testStorage().files[path], bytes);
  EXPECT_EQ(testStorage().renameCount, renamesBefore) << "value-change guard: no SD write for an unchanged cursor";
}

}  // namespace
