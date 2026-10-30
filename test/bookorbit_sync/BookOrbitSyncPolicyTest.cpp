// End-to-end tests for the REAL src/util/BookOrbitStatsSync.cpp -- the policy layer that decides
// who may capture, when the device is allowed to talk to a server about stats, and what each
// server answer means for the durable queue.
//
// This target compiles the PRODUCTION policy, protocol and queue sources together
// (BookOrbitStatsSync.cpp + BookOrbitSyncClient.cpp + BookOrbitStatsQueue.cpp +
// BookOrbitOwnerId.cpp) against a fake transport and an in-memory filesystem. Only genuine device
// dependencies are stubbed: credentials, settings, persisted app state, the RTC, SD storage, and
// the Arduino globals. That matters because the rules under test here -- "an unconfirmed server
// still gets probed but never loses data", "an unconfirmed 2xx keeps the queue" -- are properties
// of how these layers compose, and a test that reimplemented any of them could stay green while
// the shipped path rotted.
//
// EXPLICIT PROVIDER, not opportunistic detection: unlike this file's e51b383 predecessor (which
// exercised KOReaderCredentialStore + KOReaderSyncClient::Session), this exercises
// BookOrbitCredentialStore + BookOrbitSyncClient::Session directly -- BookOrbit is configured on
// its own, never inferred from a KOSync server. It also drops the old permanent-negative
// "ServerKind::OTHER" behaviour: since the server here is EXPLICITLY configured as BookOrbit by
// the user, a probe that fails to confirm it is a configuration problem to retry and surface
// (BookOrbitAuthActivity's test-connection), never a fact to cache and act on by discarding
// history -- see BookOrbitStatsSync.h's class comment.

#include <gtest/gtest.h>

#include <memory>
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

HostEspStub ESP;
HalClock halClock;

namespace {

// One identity end to end: the KOSync document hash keys the queue AND attributes the upload.
constexpr char DOC_HASH[] = "0123456789abcdef0123456789abcdef";
constexpr uint32_t NOW_MINUTES = 29372160u;  // ~2025-11, comfortably inside the plausible window

const char* const BOOKORBIT_VERSION_BODY =
    R"({"pluginVersion":"0.5.1","serverVersion":"1.2.3","capabilities":["bookmarkSync"]})";

class FakeTransport final : public KOSyncTransport {
 public:
  struct Response {
    int httpCode;
    std::string body;
  };
  struct Request {
    std::string url;
    std::string method;
    std::string payload;
  };

  explicit FakeTransport(std::vector<Response> responses) : responses_(std::move(responses)) {}

  bool begin(const std::string& url) override {
    lastUrl_ = url;
    return !url.empty();
  }
  void addHeader(const std::string&, const std::string&) override {}
  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                  const AbortCallback&) override {
    requests.push_back(
        {lastUrl_, method, payload ? std::string(reinterpret_cast<const char*>(payload), payloadLen) : ""});
    if (next_ >= responses_.size()) return -1;
    const Response& r = responses_[next_++];
    body_ = r.body;
    if (onData && !r.body.empty()) onData(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size());
    return r.httpCode;
  }
  const std::string& getString() const override { return body_; }
  void end() override {}

  std::vector<Request> requests;

 private:
  std::vector<Response> responses_;
  size_t next_ = 0;
  std::string lastUrl_;
  std::string body_;
};

struct Rig {
  FakeTransport* fake;
  BookOrbitSyncClient::Session session;
  explicit Rig(std::vector<FakeTransport::Response> responses)
      : fake(new FakeTransport(std::move(responses))), session(std::unique_ptr<KOSyncTransport>(fake)) {}
};

BookOrbitStatEvent makeEvent(const uint32_t startTime) {
  BookOrbitStatEvent e;
  e.startTime = startTime;
  e.durationSeconds = 30;
  e.page = 1234;
  e.totalPages = BookOrbitStatsQueue::PROGRESS_SCALE;
  return e;
}

// Seeds the book's queue with `count` events owned by the currently configured account, using
// the production append path.
void seedQueue(const size_t count, const uint32_t firstStart = 1893456000u) {
  std::vector<BookOrbitStatEvent> events;
  for (size_t i = 0; i < count; i++) events.push_back(makeEvent(firstStart + static_cast<uint32_t>(i) * 60u));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(DOC_HASH, BookOrbitStats::currentOwner(), events.data(), events.size()));
}

std::string acceptedBody(const int accepted) {
  return std::string(R"({"results":[{"hash":")") + DOC_HASH + R"(","accepted":)" + std::to_string(accepted) +
         R"(,"duplicates":0}],"unmatched":[]})";
}

TEST(BookOrbitInstallationIdentityLifecycleTest, MigratesLegacyIdOnlyAfterTheRandomReplacementIsDurable) {
  APP_STATE.resetToDefaults();
  APP_STATE.koStatsDeviceId = "a1b2c3d4e5f60718";  // cycle-1 deterministic hardware-id format
  APP_STATE.saveShouldFail = true;

  EXPECT_TRUE(BookOrbitStats::statsDeviceId().empty()) << "an unpersisted identity must never reach the wire";
  EXPECT_EQ(APP_STATE.koStatsDeviceId, "a1b2c3d4e5f60718") << "failed persistence must roll RAM state back too";

  APP_STATE.saveShouldFail = false;
  const std::string& migrated = BookOrbitStats::statsDeviceId();
  EXPECT_TRUE(bookOrbitStatsDeviceIdIsCurrent(migrated));
  EXPECT_EQ(APP_STATE.koStatsDeviceId, migrated);
  EXPECT_EQ(APP_STATE.saveCount, 2);
}

class BookOrbitSyncPolicyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testStorage().reset();
    BOOKORBIT_STORE.resetToDefaults();
    BOOKORBIT_STORE.setTestBaseUrl("https://books.example.test/api/v1/koreader");
    SETTINGS.resetToDefaults();
    APP_STATE.resetToDefaults();
    halClock.available = true;
    halClock.epochMinutes = NOW_MINUTES;
  }

  size_t queued() const { return BookOrbitStatsQueue::queuedCount(DOC_HASH, BookOrbitStats::currentOwner()); }
};

// ---- Capability caching, and the correctness-first guarantee ---------------------------------

TEST_F(BookOrbitSyncPolicyTest, ProbesOnceThenUploadsWhenTheServerConfirmsBookOrbit) {
  seedQueue(3);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(3)}});

  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

  ASSERT_EQ(rig.fake->requests.size(), 2u);
  EXPECT_NE(rig.fake->requests[0].url.find("/plugin/version"), std::string::npos);
  EXPECT_NE(rig.fake->requests[1].url.find("/plugin/page-stats"), std::string::npos);
  EXPECT_EQ(queued(), 0u) << "confirmed events must be removed";
  EXPECT_TRUE(BookOrbitStats::cachedCapabilities().isBookOrbit);
}

TEST_F(BookOrbitSyncPolicyTest, AConfirmedServerIsNotReprobedOnASubsequentSync) {
  seedQueue(1);
  {
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    ASSERT_TRUE(BookOrbitStats::cachedCapabilities().isBookOrbit);
  }
  seedQueue(1, 1893460000u);
  {
    Rig rig({{200, acceptedBody(1)}});  // no version probe scripted -- must not be requested
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    ASSERT_EQ(rig.fake->requests.size(), 1u);
    EXPECT_NE(rig.fake->requests[0].url.find("/plugin/page-stats"), std::string::npos);
  }
}

TEST_F(BookOrbitSyncPolicyTest, AnUnconfirmedServerIsReprobedEverySyncRatherThanSettledNegative) {
  // The behavioural core of this phase: BookOrbit is now EXPLICITLY configured, so a server that
  // fails to confirm it is retried, never permanently disabled -- unlike e51b383's
  // ServerKind::OTHER, which stopped asking (and cleared the queue) after one 404.
  seedQueue(2);
  for (int i = 0; i < 3; i++) {
    Rig rig({{404, ""}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_EQ(rig.fake->requests.size(), 1u) << "attempt " << i;
    EXPECT_FALSE(BookOrbitStats::cachedCapabilities().isBookOrbit) << "attempt " << i;
  }
  EXPECT_EQ(queued(), 2u) << "an unconfirmed server must never lose queued history";
}

TEST_F(BookOrbitSyncPolicyTest, ReadingStatsOffMeansNoProbeAndNoCapture) {
  SETTINGS.trackReadingStats = false;
  Rig rig({{200, BOOKORBIT_VERSION_BODY}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_TRUE(rig.fake->requests.empty());
  EXPECT_FALSE(BookOrbitStats::captureEnabled());
}

TEST_F(BookOrbitSyncPolicyTest, NoBookOrbitCredentialsMeansNoProbeAndNoCapture) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_TRUE(rig.fake->requests.empty());
  EXPECT_FALSE(BookOrbitStats::captureEnabled());
}

TEST_F(BookOrbitSyncPolicyTest, AnUnparseableProbeBodyLeavesTheServerUnconfirmedForALaterRetry) {
  seedQueue(1);
  for (const auto& resp : std::vector<FakeTransport::Response>{
           {-1, ""}, {401, ""}, {500, ""}, {200, R"({"pluginVersion":"0.5.1","serverVer)"}}) {
    APP_STATE.resetToDefaults();
    Rig rig({resp});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_FALSE(BookOrbitStats::cachedCapabilities().isBookOrbit) << "http " << resp.httpCode;
    EXPECT_EQ(queued(), 1u) << "http " << resp.httpCode << " must not consume the queue";
  }
}

TEST_F(BookOrbitSyncPolicyTest, ATruncatedProbeFollowedByAValidOneStillConfirmsBookOrbit) {
  seedQueue(1);
  {
    Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.2)"}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    ASSERT_FALSE(BookOrbitStats::cachedCapabilities().isBookOrbit);
  }
  {
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_TRUE(BookOrbitStats::cachedCapabilities().isBookOrbit);
    EXPECT_EQ(queued(), 0u);
  }
}

TEST_F(BookOrbitSyncPolicyTest, AGenericServerVersionResponseDoesNotEnableTelemetryButRetainsTheQueue) {
  seedQueue(1);
  for (const char* body : {R"({"serverVersion":"1.2.3"})", R"({"capabilities":["x"]})", R"({"status":"ok"})",
                           R"({"pluginVersion":1,"serverVersion":2,"capabilities":"none"})", "{}"}) {
    APP_STATE.resetToDefaults();
    Rig rig({{200, body}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_FALSE(BookOrbitStats::cachedCapabilities().isBookOrbit) << body;
    EXPECT_EQ(rig.fake->requests.size(), 1u) << "no page-stats POST may follow an unconfirmed probe: " << body;
    EXPECT_EQ(queued(), 1u) << "wrong-shaped body: " << body;
  }
}

TEST_F(BookOrbitSyncPolicyTest, CapabilitiesAreRecordedAgainstTheAccountThatWasProbed) {
  seedQueue(1);
  {
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    ASSERT_TRUE(BookOrbitStats::cachedCapabilities().isBookOrbit);
  }
  BOOKORBIT_STORE.setTestCredentials(true, "someone-else", "pw", "deadbeefdeadbeefdeadbeefdeadbeef");
  EXPECT_FALSE(BookOrbitStats::cachedCapabilities().isBookOrbit)
      << "switching account must invalidate the cached answer, not inherit it";
}

// ---- Upload outcomes and their effect on the queue -------------------------------------------

TEST_F(BookOrbitSyncPolicyTest, AnUnsupportedEndpointRetainsTheQueueRatherThanClearingIt) {
  // Unlike e51b383: this server was EXPLICITLY configured as BookOrbit, so an absent page-stats
  // endpoint (e.g. an older BookOrbit version) is a configuration/version problem to surface, not
  // a settled fact -- deleting the user's not-yet-uploaded reading history over it would be wrong.
  seedQueue(2);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {404, ""}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

  EXPECT_EQ(queued(), 2u) << "an explicitly-configured server's missing endpoint must not destroy history";
}

TEST_F(BookOrbitSyncPolicyTest, AnUnmatchedBookRetainsTheQueueForALaterAttempt) {
  seedQueue(2);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, std::string(R"({"results":[],"unmatched":[")") + DOC_HASH + R"("]})"}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(queued(), 2u) << "the book may be added to the library later";
}

TEST_F(BookOrbitSyncPolicyTest, AnUnconfirmedTwoHundredRetainsTheQueue) {
  seedQueue(2);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, "<html>ok</html>"}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(queued(), 2u) << "retrying is free under the server's dedup key; deleting is permanent";
}

TEST_F(BookOrbitSyncPolicyTest, AnOversizedBatchIsRetriedSmallerWithoutDeletingAnything) {
  seedQueue(4);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {413, ""}, {200, acceptedBody(1)}, {200, acceptedBody(1)}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

  EXPECT_EQ(queued(), 2u);
  ASSERT_GE(rig.fake->requests.size(), 3u) << "probe, the oversized batch, then a smaller retry";
}

TEST_F(BookOrbitSyncPolicyTest, ASingleEventRejectionRetainsTheEventRatherThanDroppingIt) {
  seedQueue(3);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {413, ""}, {413, ""}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

  EXPECT_EQ(queued(), 3u) << "a rejected single-event request is not proof that the event is bad";
}

TEST_F(BookOrbitSyncPolicyTest, RepeatedRequestRejectionsCanNeverReduceTheQueue) {
  for (const int code : {400, 413, 422}) {
    testStorage().reset();
    APP_STATE.resetToDefaults();
    seedQueue(5);
    ASSERT_EQ(queued(), 5u);

    for (int sync = 0; sync < 4; sync++) {
      Rig rig({{200, BOOKORBIT_VERSION_BODY}, {code, ""}, {code, ""}, {code, ""}, {code, ""}});
      BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
      EXPECT_EQ(queued(), 5u) << "http " << code << " reduced the queue on sync " << sync;
    }
  }
}

TEST_F(BookOrbitSyncPolicyTest, A400OrA422IsDeferredWithoutEvenTryingASmallerRequest) {
  for (const int code : {400, 422}) {
    testStorage().reset();
    APP_STATE.resetToDefaults();
    seedQueue(4);
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {code, ""}, {200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

    EXPECT_EQ(queued(), 4u) << "http " << code;
    EXPECT_EQ(rig.fake->requests.size(), 2u) << "http " << code << ": probe and one rejected POST, then defer";
  }
}

TEST_F(BookOrbitSyncPolicyTest, AQueueRetainedThroughRejectionsStillUploadsOnceTheServerRecovers) {
  seedQueue(3);
  {
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {422, ""}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    ASSERT_EQ(queued(), 3u);
  }
  {
    Rig rig({{200, acceptedBody(3)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_EQ(queued(), 0u) << "the deferred events must still be deliverable";
  }
}

TEST_F(BookOrbitSyncPolicyTest, APartialAcknowledgementRetainsTheWholeBatch) {
  seedQueue(3);
  Rig rig({{200, BOOKORBIT_VERSION_BODY},
           {200, std::string(R"({"results":[{"hash":")") + DOC_HASH +
                     R"(","accepted":1,"duplicates":0}],"unmatched":[]})"}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(queued(), 3u) << "1 of 3 acknowledged is not confirmation of 3";
}

TEST_F(BookOrbitSyncPolicyTest, DuplicatesCountTowardsConfirmation) {
  seedQueue(3);
  Rig rig({{200, BOOKORBIT_VERSION_BODY},
           {200, std::string(R"({"results":[{"hash":")") + DOC_HASH +
                     R"(","accepted":0,"duplicates":3}],"unmatched":[]})"}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(queued(), 0u);
}

TEST_F(BookOrbitSyncPolicyTest, RetryableFailuresKeepEverything) {
  for (const int code : {401, 500, 503, -1}) {
    testStorage().reset();
    APP_STATE.resetToDefaults();
    seedQueue(2);
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {code, ""}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
    EXPECT_EQ(queued(), 2u) << "http " << code;
  }
}

TEST_F(BookOrbitSyncPolicyTest, StaleEventsAreDroppedWithoutSpendingARequest) {
  const uint32_t nowSeconds = NOW_MINUTES * 60u;
  seedQueue(2, nowSeconds - BookOrbitStatsQueue::MAX_EVENT_AGE_SECONDS - 10'000u);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}});

  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(rig.fake->requests.size(), 1u) << "only the probe; month-old backlog is not worth uploading";
  EXPECT_EQ(queued(), 0u);
}

TEST_F(BookOrbitSyncPolicyTest, AnAbsentRtcDisablesTheAgeCheckRatherThanTheUpload) {
  seedQueue(1, 1000u);  // ancient by any clock
  halClock.available = false;
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});

  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(rig.fake->requests.size(), 2u) << "with no clock the event is uploaded, not silently binned";
  EXPECT_EQ(queued(), 0u);
}

TEST_F(BookOrbitSyncPolicyTest, TheUploadCarriesThePerDeviceStatsIdNotTheGenericKoSyncConstant) {
  seedQueue(1);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);

  ASSERT_EQ(rig.fake->requests.size(), 2u);
  const std::string& payload = rig.fake->requests[1].payload;
  EXPECT_NE(payload.find(BookOrbitStats::statsDeviceId()), std::string::npos);
  EXPECT_EQ(payload.find(std::string("\"deviceId\":\"") + BookOrbitLimits::DEVICE_ID + "\""), std::string::npos)
      << "the generic-KOSync shared device_id would merge every device's BookOrbit sessions server-side";
}

TEST_F(BookOrbitSyncPolicyTest, EmptyQueueCostsNothingBeyondTheOneTimeProbe) {
  Rig rig({{200, BOOKORBIT_VERSION_BODY}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  EXPECT_EQ(rig.fake->requests.size(), 1u) << "no page-stats POST for an empty queue";
}

TEST_F(BookOrbitSyncPolicyTest, AMissingDocumentIdentityIsANoOp) {
  seedQueue(1);
  Rig rig({{200, BOOKORBIT_VERSION_BODY}});
  BookOrbitStats::uploadQueued(rig.session, "");
  EXPECT_TRUE(rig.fake->requests.empty()) << "without a book identity there is nothing to key or attribute";
}

TEST_F(BookOrbitSyncPolicyTest, DrainPossibleGatesTheStatsIdentityDerivation) {
  // Callers check this BEFORE deriving the stats document id, because that derivation reads
  // chunks from the book file.
  EXPECT_TRUE(BookOrbitStats::drainPossible()) << "an unconfirmed server may still turn out to be BookOrbit";

  SETTINGS.trackReadingStats = false;
  EXPECT_FALSE(BookOrbitStats::drainPossible());
  SETTINGS.trackReadingStats = true;

  BOOKORBIT_STORE.setTestCredentials(false);
  EXPECT_FALSE(BookOrbitStats::drainPossible());
}

TEST_F(BookOrbitSyncPolicyTest, DrainPossibleIsGatedWhileALegacyMigrationIsPending) {
  // Regression: capture/drain ran even while a legacy-KOSync migration was in progress
  // (credentials already copied, but the queue-owner rewrite not yet committed). appendBatch()'s
  // own "different owner -> replace" policy would treat the still-legacy-owned queue as a
  // stranger's leftover and destructively promote a fresh one over it, erasing whatever legacy
  // events the migration was about to re-own.
  APP_STATE.boCredentialsMigrated = true;
  APP_STATE.boMigrationAttempted = false;  // migration in progress, not yet committed
  EXPECT_FALSE(BookOrbitStats::drainPossible()) << "must not drain while the migration could still reassign the queue";

  APP_STATE.boMigrationAttempted = true;  // migration committed (or abandoned) -- no longer pending
  EXPECT_TRUE(BookOrbitStats::drainPossible());

  APP_STATE.boCredentialsMigrated = false;  // never migrated at all (e.g. a fresh BookOrbit setup)
  APP_STATE.boMigrationAttempted = false;
  EXPECT_TRUE(BookOrbitStats::drainPossible()) << "boMigrationAttempted alone is not \"pending\" -- both must hold";
}

TEST_F(BookOrbitSyncPolicyTest, CapabilityRecordingCannotClobberTheLegacyOwnerWhileMigrationIsPending) {
  // Progress and bookmark paths share recordCapabilities(). If either probes successfully after
  // a queue-rewrite failure in this boot, replacing koStatsServerId with the new owner would make
  // the next boot mistake the original legacy confirmation for stale state and abandon the
  // still-pending queue migration.
  APP_STATE.boCredentialsMigrated = true;
  APP_STATE.boMigrationAttempted = false;
  APP_STATE.koStatsServerId = "11111111111111111111111111111111";
  APP_STATE.koStatsServerKind = 1;
  const int savesBefore = APP_STATE.saveCount;

  BookOrbitSyncClient::Capabilities caps;
  caps.isBookOrbit = true;
  caps.hasBookmarkSync = true;
  caps.hasCatalogBulkManifest = true;
  caps.hasCatalogDashboardSections = true;
  BookOrbitStats::recordCapabilities(caps);

  EXPECT_EQ(APP_STATE.koStatsServerId, "11111111111111111111111111111111");
  EXPECT_EQ(APP_STATE.koStatsServerKind, 1);
  EXPECT_FALSE(APP_STATE.koStatsCapBookmarkSync);
  EXPECT_FALSE(APP_STATE.koStatsCapCatalogBulkManifest);
  EXPECT_FALSE(APP_STATE.koStatsCapCatalogDashboardSections);
  EXPECT_EQ(APP_STATE.saveCount, savesBefore) << "the pending migration marker must remain byte-for-byte intact";
}

// Note: the no-session convenience overload (used by every production call site --
// KOReaderAutoSync.cpp, KOReaderSyncActivity.cpp) is intentionally NOT exercised here. It
// constructs a real BookOrbitSyncClient::Session via makeDefaultKOSyncTransport(), which is
// deliberately not linked into this target (see this directory's CMakeLists.txt) so the suite
// never drags in the real freeink::SecureHttpClient/WiFiClient/wolfSSL stack -- calling it would
// break that isolation for no behavioural gain, since it is a three-line pass-through onto the
// session-accepting overload already covered by every test above.

// ---- Owner isolation across the whole policy layer --------------------------------------------

TEST_F(BookOrbitSyncPolicyTest, AnotherAccountsQueueIsNeverUploadedUnderTheCurrentCredentials) {
  seedQueue(3);
  ASSERT_EQ(queued(), 3u);

  BOOKORBIT_STORE.setTestCredentials(true, "other-user", "pw", "deadbeefdeadbeefdeadbeefdeadbeef");
  EXPECT_EQ(queued(), 0u);

  Rig rig({{200, BOOKORBIT_VERSION_BODY}});
  BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  ASSERT_EQ(rig.fake->requests.size(), 1u) << "probe only; there is nothing this account may send";
}

TEST_F(BookOrbitSyncPolicyTest, DetectionResultIsPersistedOnceRatherThanOnEverySync) {
  seedQueue(1);
  {
    Rig rig({{200, BOOKORBIT_VERSION_BODY}, {200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  }
  const int savesAfterFirst = APP_STATE.saveCount;
  EXPECT_GE(savesAfterFirst, 1);

  seedQueue(1, 1893460000u);
  {
    Rig rig({{200, acceptedBody(1)}});
    BookOrbitStats::uploadQueued(rig.session, DOC_HASH);
  }
  EXPECT_EQ(APP_STATE.saveCount, savesAfterFirst) << "an unchanged answer must not rewrite state.json";
}

}  // namespace

// ---- Firmware-only symbols, supplied for the host link ----------------------------------------
// BookOrbitOwnerId::derive(url, user), bookOrbitFirmwareSha256(), and the firmware RNG binding live in the mbedTLS/ESP
// translation unit, which cannot build here. Defining them in the test binary means the
// production policy code under test calls exactly the functions it calls on device; only the
// digest behind them is a deterministic stand-in rather than SHA-256.
namespace {
void hostDigest(const uint8_t* data, const size_t len, uint8_t out[32]) {
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
}  // namespace

BookOrbitSha256Fn bookOrbitFirmwareSha256() { return &hostDigest; }

void bookOrbitFirmwareRandom(uint8_t* out, const size_t len) {
  for (size_t i = 0; i < len; i++) out[i] = static_cast<uint8_t>(i * 17u + 3u);
}

BookOrbitOwnerId BookOrbitOwnerId::derive(const std::string& baseUrl, const std::string& username) {
  return derive(baseUrl, username, &hostDigest);
}
