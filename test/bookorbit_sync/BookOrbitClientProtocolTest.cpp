// Protocol tests for the REAL lib/KOReaderSync/BookOrbitSyncClient.cpp (compiled directly, not
// reimplemented) against a fake, in-memory KOSyncTransport -- no network, WiFi or TLS.
//
// This is the direct successor to test/bookorbit_stats/BookOrbitProtocolTest.cpp (removed when
// detectBookOrbit()/uploadPageStats() moved off KOReaderSyncClient::Session onto their own
// BookOrbitSyncClient -- see docs/bookorbit-feature-matrix.md). Two things are under test: that
// BookOrbit is identified by CAPABILITY rather than by hostname, and that each server answer maps
// onto the right queue consequence (keep, drop, or stop asking). Unlike the predecessor test, a
// probe that does not confirm BookOrbit is no longer treated as a successful negative detection --
// this client is only ever used against a server the user explicitly configured as BookOrbit, so
// UNSUPPORTED/NOT_BOOKORBIT are configuration signals, not cacheable facts (see
// BookOrbitStatsSync.cpp, tested separately in BookOrbitSyncPolicyTest.cpp).

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitBookmarks.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitStats.h"
#include "BookOrbitSyncClient.h"
#include "KOSyncTransport.h"
#include "Logging.h"

namespace {

constexpr char DOC_HASH[] = "0123456789abcdef0123456789abcdef";  // 32 hex, as the server requires
constexpr char DEVICE_ID[] = "a1b2c3d4e5f60718";                 // per-device stats id; see bookOrbitStatsDeviceId()

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

  bool beginSucceeds = true;

  bool begin(const std::string& url) override {
    lastUrl_ = url;
    return beginSucceeds && !url.empty();
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
      onData(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size());
    }
    return resp.httpCode;
  }

  const std::string& getString() const override { return lastBody_; }
  void end() override {}

  std::vector<RecordedRequest> requests;
  std::vector<std::pair<std::string, std::string>> headers;

 private:
  std::vector<ScriptedResponse> responses_;
  size_t nextResponse_ = 0;
  std::string lastUrl_;
  std::string lastBody_;
};

// Keeps a borrowed pointer to the fake so a test can inspect its call log after handing
// ownership to the Session.
struct Rig {
  FakeKOSyncTransport* fake;
  BookOrbitSyncClient::Session session;

  explicit Rig(std::vector<FakeKOSyncTransport::ScriptedResponse> responses)
      : fake(new FakeKOSyncTransport(std::move(responses))), session(std::unique_ptr<KOSyncTransport>(fake)) {}
};

std::vector<BookOrbitStatEvent> sampleEvents(const size_t count = 2) {
  std::vector<BookOrbitStatEvent> events;
  for (size_t i = 0; i < count; i++) {
    BookOrbitStatEvent e;
    e.startTime = 1893456000u + static_cast<uint32_t>(i) * 60u;
    e.durationSeconds = static_cast<uint16_t>(30 + i);
    e.page = static_cast<uint16_t>(1000 + i);
    e.totalPages = 10000;
    events.push_back(e);
  }
  return events;
}

class BookOrbitClientProtocolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    BOOKORBIT_STORE.resetToDefaults();
    BOOKORBIT_STORE.setTestBaseUrl("https://books.example.test/api/v1/koreader");
    CapturedLogs::clear();
  }
};

// ---- Capability probe ---------------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, ConfirmsBookOrbitFromTheVersionEndpointsPayload) {
  Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.2.3","capabilities":["bookmarkSync"]})"}});
  BookOrbitSyncClient::Capabilities caps;
  EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::OK);
  EXPECT_TRUE(caps.isBookOrbit);
  EXPECT_TRUE(caps.hasBookmarkSync);
  EXPECT_FALSE(caps.hasCatalogBulkManifest);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].method, "GET");
  EXPECT_EQ(rig.fake->requests[0].url, "https://books.example.test/api/v1/koreader/plugin/version")
      << "the probe must hang off the CONFIGURED base URL, never a hardcoded public host";
}

TEST_F(BookOrbitClientProtocolTest, UnrecognizedCapabilityStringsAreIgnoredNotStored) {
  Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.2.3","capabilities":["someFutureThing"]})"}});
  BookOrbitSyncClient::Capabilities caps;
  EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::OK);
  EXPECT_TRUE(caps.isBookOrbit);
  EXPECT_FALSE(caps.hasBookmarkSync);
  EXPECT_FALSE(caps.hasCatalogBulkManifest);
  EXPECT_FALSE(caps.hasCatalogDashboardSections);
}

TEST_F(BookOrbitClientProtocolTest, SelfHostedBookOrbitOnAnyDomainIsStillConfirmed) {
  BOOKORBIT_STORE.setTestBaseUrl("http://192.168.1.50:3000/api/v1/koreader");
  Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.0.0","capabilities":[]})"}});
  BookOrbitSyncClient::Capabilities caps;
  EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::OK);
  EXPECT_TRUE(caps.isBookOrbit) << "identity must come from the capability probe, not the hostname";
}

TEST_F(BookOrbitClientProtocolTest, MissingEndpointIsUnsupportedNotANegativeDetection) {
  // Unlike a hostname-inferred KOSync setup, this client is only ever used against a server the
  // user explicitly configured as BookOrbit -- so an absent endpoint is a configuration signal
  // to surface (BookOrbitAuthActivity's test-connection), not a fact worth caching.
  for (const int code : {404, 405, 501}) {
    Rig rig({{code, ""}});
    BookOrbitSyncClient::Capabilities caps;
    EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::UNSUPPORTED) << "code " << code;
    EXPECT_FALSE(caps.isBookOrbit) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, AWellFormedNonBookOrbitBodyIsNotBookOrbit) {
  // Parseable JSON that simply is not KoreaderPluginVersionInfo. Note the near-misses -- a lone
  // serverVersion, or the right field names carrying the wrong TYPES -- must not confirm either.
  for (const char* body : {"{}", R"({"status":"ok"})", R"({"serverVersion":"1.2.3"})", R"({"capabilities":["x"]})",
                           R"({"pluginVersion":"0.5","serverVersion":"1.0"})",
                           R"({"pluginVersion":1,"serverVersion":2,"capabilities":[]})",
                           R"({"pluginVersion":"a","serverVersion":"b","capabilities":"not-an-array"})"}) {
    Rig rig({{200, body}});
    BookOrbitSyncClient::Capabilities caps;
    EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::NOT_BOOKORBIT) << "body: " << body;
    EXPECT_FALSE(caps.isBookOrbit) << "body: " << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, AnUnparseableOrTruncatedTwoHundredIsAJsonError) {
  // A reverse proxy's HTML, an empty body, or a response cut short mid-JSON tells us nothing --
  // must not be reported as NOT_BOOKORBIT, which the caller could otherwise treat as definitive.
  for (const char* body :
       {"<!DOCTYPE html><html><body>Not found</body></html>", "", R"({"pluginVersion":"0.5.1","serverVersion":"1.2)"}) {
    Rig rig({{200, body}});
    BookOrbitSyncClient::Capabilities caps;
    EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::JSON_ERROR) << "body: " << body;
    EXPECT_FALSE(caps.isBookOrbit) << "body: " << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, TransientFailuresAreDistinctFromAConfigurationProblem) {
  const std::vector<std::pair<int, BookOrbitSyncClient::Error>> cases = {
      {-1, BookOrbitSyncClient::NETWORK_ERROR},
      {401, BookOrbitSyncClient::AUTH_FAILED},
      {500, BookOrbitSyncClient::SERVER_ERROR},
      {503, BookOrbitSyncClient::SERVER_ERROR},
  };
  for (const auto& [code, expected] : cases) {
    Rig rig({{code, ""}});
    BookOrbitSyncClient::Capabilities caps;
    EXPECT_EQ(rig.session.probeCapabilities(caps), expected) << "code " << code;
    EXPECT_FALSE(caps.isBookOrbit) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, ProbeSendsTheConfiguredBookOrbitCredentials) {
  Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.0.0","capabilities":[]})"}});
  BookOrbitSyncClient::Capabilities caps;
  ASSERT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::OK);

  bool sawUser = false, sawKey = false, sawBasic = false;
  for (const auto& [name, value] : rig.fake->headers) {
    if (name == "x-auth-user" && value == "bouser") sawUser = true;
    if (name == "x-auth-key" && value == "cafebabecafebabecafebabecafebabe") sawKey = true;
    if (name == "Authorization" && value.rfind("Basic ", 0) == 0) sawBasic = true;
  }
  EXPECT_TRUE(sawUser && sawKey && sawBasic) << "must use BookOrbitCredentialStore, never KOReaderCredentialStore";
}

TEST_F(BookOrbitClientProtocolTest, NoCredentialsMeansNoProbeRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, R"({"pluginVersion":"0.5.1","serverVersion":"1.0.0","capabilities":[]})"}});
  BookOrbitSyncClient::Capabilities caps;
  EXPECT_EQ(rig.session.probeCapabilities(caps), BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_FALSE(caps.isBookOrbit);
  EXPECT_TRUE(rig.fake->requests.empty());
}

// ---- authenticate() -------------------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, AuthenticateSucceedsOnExplicitOk) {
  Rig rig({{200, R"({"authorized":"OK"})"}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::OK);
}

// Regression test: `doc["authorized"] | nullptr` looks like it reads the field, but ArduinoJson
// resolves the bare `nullptr` literal to the generic templated operator| (an exact match for
// T=std::nullptr_t beats the const-char*-specific overload, which would only be reached via an
// implicit conversion) -- so it always evaluated to null regardless of the field's actual value,
// and this explicit-denial branch was silently unreachable. Fixed by
// `doc["authorized"] | static_cast<const char*>(nullptr)`, which forces the intended overload.
TEST_F(BookOrbitClientProtocolTest, AuthenticateFailsOnExplicitDenial) {
  Rig rig({{200, R"({"authorized":"DENIED"})"}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::AUTH_FAILED);
}

TEST_F(BookOrbitClientProtocolTest, AuthenticateTreatsAMissingAuthorizedFieldAsSuccess) {
  // No explicit denial to react to; a 2xx with no opinion on the field is not itself a failure.
  Rig rig({{200, R"({"someOtherField":true})"}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::OK);
}

TEST_F(BookOrbitClientProtocolTest, AuthenticateRejectsUnparseableBody) {
  Rig rig({{200, "not json"}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, AuthenticateMapsHttpFailuresLikeProbeCapabilities) {
  Rig rig({{401, ""}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::AUTH_FAILED);
}

TEST_F(BookOrbitClientProtocolTest, AuthenticateNoCredentialsMeansNoRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, R"({"authorized":"OK"})"}});
  EXPECT_EQ(rig.session.authenticate(), BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

// ---- Progress sync -------------------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, GetProgressParsesTheDocumentFields) {
  Rig rig({{200, R"({"document":"0123456789ABCDEF0123456789ABCDEF","progress":"/body/DocFragment[5]",
                    "percentage":0.42,"device":"Kobo Clara","device_id":"some-kobo-id",
                    "timestamp":1893456000})"}});
  BookOrbitProgress progress;
  ASSERT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::OK);
  EXPECT_EQ(progress.document, DOC_HASH) << "the validated requested identity stays canonical locally";
  EXPECT_EQ(progress.progress, "/body/DocFragment[5]");
  EXPECT_FLOAT_EQ(progress.percentage, 0.42f);
  EXPECT_EQ(progress.device, "Kobo Clara");
  EXPECT_EQ(progress.timestamp, 1893456000);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].method, "GET");
  EXPECT_EQ(rig.fake->requests[0].url,
            std::string("https://books.example.test/api/v1/koreader/syncs/progress/") + DOC_HASH);
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsMissingInvalidOrMillisecondTimestamp) {
  for (const char* body : {
           R"({"progress":"/body/p[1]","percentage":0.42})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":0})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":1893456000000})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":"1893456000"})",
       }) {
    Rig rig({{200, body}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR) << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, WebResetRecognitionRequiresTheExactServerIdentityAndStartPosition) {
  BookOrbitProgress reset;
  reset.progress = "/body/DocFragment[1]/body";
  reset.percentage = 0.0f;
  reset.device = "web";
  reset.deviceId = "bookorbit-web";
  EXPECT_TRUE(reset.isWebReset());
  reset.deviceId = "another-device";
  EXPECT_FALSE(reset.isWebReset());
  reset.deviceId = "bookorbit-web";
  reset.progress = "/body/DocFragment[2]/body";
  EXPECT_FALSE(reset.isWebReset());
}

TEST_F(BookOrbitClientProtocolTest, GetProgressTreats404And204AsNotFoundNotAnError) {
  for (const int code : {404, 204}) {
    Rig rig({{code, ""}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::NOT_FOUND) << "code " << code;
    EXPECT_TRUE(progress.progress.empty()) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, GetProgressNeverParsesA204BodyEvenIfOneIsSent) {
  // No HTTP semantics allow a 204 to carry a body, but a misbehaving server could still send
  // bytes -- must not reach the JSON parser regardless.
  Rig rig({{204, R"({"progress":"/should/not/be/read","percentage":0.9})"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::NOT_FOUND);
  EXPECT_TRUE(progress.progress.empty());
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsUnparseableBody) {
  Rig rig({{200, "not json"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, GetProgressTreatsAnEmptyTwoHundredObjectAsNotFound) {
  // The official server's own no-progress contract (koreader.controller.ts: `return progress ??
  // {}`) is an HTTP 200 carrying an empty JSON object, not a 404/204 -- must be treated exactly
  // like those, not as a malformed/wrong-shaped response.
  Rig rig({{200, "{}"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::NOT_FOUND);
  EXPECT_TRUE(progress.progress.empty());
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsANonObjectBody) {
  Rig rig({{200, R"(["not","an","object"])"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsAMissingPercentageField) {
  Rig rig({{200, R"({"progress":"/x"})"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsANonNumericPercentageField) {
  Rig rig({{200, R"({"progress":"/x","percentage":"not-a-number"})"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsAnOutOfRangePercentageField) {
  for (const char* body : {R"({"progress":"/x","percentage":-0.1})", R"({"progress":"/x","percentage":1.1})"}) {
    Rig rig({{200, body}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR) << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, GetProgressAcceptsAPresentZeroPercentageAsARealPosition) {
  // A reset-to-start position is a legitimate, meaningful percentage of exactly zero -- must
  // never be conflated with "no progress recorded" (that is the empty-object case above).
  Rig rig({{200, R"({"progress":"/body/DocFragment[1]","percentage":0.0,"timestamp":1893456000})"}});
  BookOrbitProgress progress;
  ASSERT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::OK);
  EXPECT_FLOAT_EQ(progress.percentage, 0.0f);
  EXPECT_EQ(progress.progress, "/body/DocFragment[1]");
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsAMissingOrNonStringProgressField) {
  for (const char* body : {R"({"percentage":0.5})", R"({"progress":42,"percentage":0.5})"}) {
    Rig rig({{200, body}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR) << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, GetProgressRejectsAMismatchedOrMistypedEchoedDocument) {
  for (const char* body : {
           R"({"document":"ffffffffffffffffffffffffffffffff","progress":"/x","percentage":0.5})",
           R"({"document":42,"progress":"/x","percentage":0.5})",
           R"({"document":null,"progress":"/x","percentage":0.5})",
       }) {
    Rig rig({{200, body}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::JSON_ERROR) << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, GetProgressMapsHttpFailures) {
  const std::vector<std::pair<int, BookOrbitSyncClient::Error>> cases = {
      {-1, BookOrbitSyncClient::NETWORK_ERROR},
      {401, BookOrbitSyncClient::AUTH_FAILED},
      {500, BookOrbitSyncClient::SERVER_ERROR},
  };
  for (const auto& [code, expected] : cases) {
    Rig rig({{code, ""}});
    BookOrbitProgress progress;
    EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), expected) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, GetProgressNoCredentialsMeansNoRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, R"({"progress":"x","percentage":0.1})"}});
  BookOrbitProgress progress;
  EXPECT_EQ(rig.session.getProgress(DOC_HASH, progress), BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressSendsTheDocumentDeviceAndPosition) {
  Rig rig({{200, ""}});
  BookOrbitProgress progress;
  progress.document = DOC_HASH;
  progress.progress = "/body/DocFragment[9]";
  progress.percentage = 0.77f;
  progress.device = "CrossFade X3";
  progress.deviceId = "device-abc123";
  progress.timestamp = 1893456000;
  ASSERT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::OK);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].method, "PUT");
  EXPECT_EQ(rig.fake->requests[0].url, "https://books.example.test/api/v1/koreader/syncs/progress");

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_STREQ(doc["document"], DOC_HASH);
  EXPECT_STREQ(doc["progress"], "/body/DocFragment[9]");
  EXPECT_FLOAT_EQ(doc["percentage"].as<float>(), 0.77f);
  EXPECT_STREQ(doc["device"], "CrossFade X3");
  // Per-install id, NOT BookOrbitLimits::DEVICE_ID's shared literal: the official
  // BookOrbit server keys a per-device progress row and a reset-convergence hold on this value
  // (koreader.service.ts upsertDeviceProgress/resolveResetHold), so every device sharing the old
  // constant would have their reset-convergence state cross-contaminated.
  EXPECT_STREQ(doc["device_id"], "device-abc123");
  EXPECT_EQ(doc["timestamp"].as<int64_t>(), 1893456000);
  EXPECT_STRNE(doc["device_id"], BookOrbitLimits::DEVICE_ID);
}

TEST_F(BookOrbitClientProtocolTest, VerifiedPushRejectsReceiptOnlyAckWithDifferentSelectedPosition) {
  BookOrbitProgress local;
  local.document = DOC_HASH;
  local.progress = "/body/DocFragment[2]/body/p[3]";
  local.percentage = 0.402793f;
  local.deviceId = DEVICE_ID;
  local.timestamp = 1791072000;
  // Deployed API's acknowledgement contains no saved position. This GET can also be
  // a concurrent newer reader; rejecting it must never force another PUT.
  Rig rig({{200, R"({"document":"0123456789abcdef0123456789abcdef","timestamp":1791072000})"},
           {200, R"({"progress":"/body/DocFragment[3]/body","percentage":0.3024,"timestamp":1791072001})"}});
  EXPECT_EQ(rig.session.updateProgressAndVerify(local), BookOrbitSyncClient::PROGRESS_NOT_CONFIRMED);
  ASSERT_EQ(rig.fake->requests.size(), 2u);
  EXPECT_EQ(rig.fake->requests[0].method, "PUT");
  EXPECT_EQ(rig.fake->requests[1].method, "GET");
}

TEST_F(BookOrbitClientProtocolTest, VerifiedPushRequiresPositionPercentageAndOriginTime) {
  BookOrbitProgress local;
  local.document = DOC_HASH;
  local.progress = "/body/DocFragment[2]/body/p[3]";
  local.percentage = 0.25f;  // A newest event may move BACKWARD from remote 0.40.
  local.deviceId = DEVICE_ID;
  local.timestamp = 1791072000;
  const std::vector<std::pair<std::string, BookOrbitSyncClient::Error>> cases = {
      {R"({"progress":"/body/DocFragment[2]/body/p[3]","percentage":0.25,"timestamp":1791072000})", BookOrbitSyncClient::OK},
      {R"({"progress":"/body/DocFragment[2]/body/p[3]","percentage":0.4,"timestamp":1791072000})", BookOrbitSyncClient::PROGRESS_NOT_CONFIRMED},
      {R"({"progress":"/body/DocFragment[2]/body/p[3]","percentage":0.25,"timestamp":1791071999})", BookOrbitSyncClient::PROGRESS_NOT_CONFIRMED},
      {R"({"progress":"/body/DocFragment[1]/body","percentage":0,"timestamp":1791072001,"device":"web","device_id":"bookorbit-web"})", BookOrbitSyncClient::PROGRESS_NOT_CONFIRMED},
      {"{}", BookOrbitSyncClient::PROGRESS_NOT_CONFIRMED},
  };
  for (const auto& [body, expected] : cases) {
    Rig rig({{200, "{}"}, {200, body}});
    EXPECT_EQ(rig.session.updateProgressAndVerify(local), expected) << body;
    EXPECT_EQ(rig.fake->requests.size(), 2u);
  }
}

TEST_F(BookOrbitClientProtocolTest, VerifiedPushPropagatesReadbackFailuresWithoutAnotherWrite) {
  BookOrbitProgress local;
  local.document = DOC_HASH;
  local.progress = "/body/DocFragment[2]/body";
  local.percentage = 0.25f;
  local.deviceId = DEVICE_ID;
  local.timestamp = 1791072000;
  for (const auto& [code, expected] : std::vector<std::pair<int, BookOrbitSyncClient::Error>>{
           {-1, BookOrbitSyncClient::NETWORK_ERROR}, {401, BookOrbitSyncClient::AUTH_FAILED},
           {500, BookOrbitSyncClient::SERVER_ERROR}}) {
    Rig rig({{200, "{}"}, {code, ""}});
    EXPECT_EQ(rig.session.updateProgressAndVerify(local), expected);
    EXPECT_EQ(rig.fake->requests.size(), 2u);
  }
  Rig failedPut({{401, ""}});
  EXPECT_EQ(failedPut.session.updateProgressAndVerify(local), BookOrbitSyncClient::AUTH_FAILED);
  EXPECT_EQ(failedPut.fake->requests.size(), 1u);
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressRejectsAnEmptyInstallationId) {
  Rig rig({{200, ""}});
  BookOrbitProgress progress;
  progress.document = DOC_HASH;
  progress.progress = "/x";
  progress.percentage = 0.1f;
  EXPECT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressRejectsMissingOrMillisecondOriginTimestampBeforeNetwork) {
  for (const int64_t timestamp : {0LL, 1893456000000LL}) {
    Rig rig({{200, ""}});
    BookOrbitProgress progress;
    progress.document = DOC_HASH;
    progress.progress = "/x";
    progress.percentage = 0.1f;
    progress.deviceId = DEVICE_ID;
    progress.timestamp = timestamp;
    EXPECT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::BAD_REQUEST);
    EXPECT_TRUE(rig.fake->requests.empty());
  }
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressFallsBackToADeviceNameWhenNoneIsSet) {
  Rig rig({{200, ""}});
  BookOrbitProgress progress;
  progress.document = DOC_HASH;
  progress.progress = "/body/DocFragment[1]";
  progress.percentage = 0.1f;
  progress.deviceId = DEVICE_ID;
  progress.timestamp = 1893456000;
  ASSERT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::OK);

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_FALSE(std::string(doc["device"].as<const char*>()).empty());
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressRefusesAHashThatIsNotThirtyTwoHexCharacters) {
  Rig rig({{200, ""}});
  BookOrbitProgress progress;
  progress.document = "tooshort";
  progress.progress = "/x";
  progress.percentage = 0.1f;
  EXPECT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressMapsHttpFailures) {
  const std::vector<std::pair<int, BookOrbitSyncClient::Error>> cases = {
      {-1, BookOrbitSyncClient::NETWORK_ERROR},
      {401, BookOrbitSyncClient::AUTH_FAILED},
      {500, BookOrbitSyncClient::SERVER_ERROR},
  };
  for (const auto& [code, expected] : cases) {
    Rig rig({{code, ""}});
    BookOrbitProgress progress;
    progress.document = DOC_HASH;
    progress.progress = "/x";
    progress.percentage = 0.1f;
    progress.deviceId = DEVICE_ID;
    progress.timestamp = 1893456000;
    EXPECT_EQ(rig.session.updateProgress(progress), expected) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, UpdateProgressNoCredentialsMeansNoRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, ""}});
  BookOrbitProgress progress;
  progress.document = DOC_HASH;
  progress.progress = "/x";
  progress.percentage = 0.1f;
  EXPECT_EQ(rig.session.updateProgress(progress), BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

// ---- Bookmark exchange -----------------------------------------------------------------------

BookOrbitBookmarkKeyRow makeKeyRow(const char* datetime, const char* pos) {
  BookOrbitBookmarkKeyRow row;
  char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  EXPECT_TRUE(bookOrbitBookmarkKey(datetime, pos, k));
  memcpy(row.k, k, sizeof(row.k));
  snprintf(row.dt, sizeof(row.dt), "%s", datetime);
  return row;
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksSendsKeysAndChangesInTheRequestBody) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":[],"delete":[]}}]})"}});
  const BookOrbitBookmarkKeyRow keys[] = {makeKeyRow("2020-01-01 00:00:00", "/a")};
  BookOrbitBookmark changes[1];
  snprintf(changes[0].datetime, sizeof(changes[0].datetime), "2020-01-01 00:00:00");
  changes[0].pos = "/a";
  changes[0].pageno = 5;

  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "Xteink X3", keys, 1, /*keysComplete=*/true, changes, 1,
                                          outUnmatched, incoming, outMorePending),
            BookOrbitSyncClient::OK);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].method, "POST");
  EXPECT_EQ(rig.fake->requests[0].url, "https://books.example.test/api/v1/koreader/plugin/bookmarks/exchange");

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_STREQ(doc["deviceId"], DEVICE_ID)
      << "must be the per-installation unique id, not BookOrbitLimits::DEVICE_ID's shared literal";
  EXPECT_STREQ(doc["deviceModel"], "Xteink X3");
  EXPECT_STREQ(doc["pluginVersion"], BookOrbitLimits::PLUGIN_VERSION);
  ASSERT_EQ(doc["books"].size(), 1u);
  EXPECT_STREQ(doc["books"][0]["hash"], DOC_HASH);
  EXPECT_TRUE(doc["books"][0]["keysComplete"].as<bool>());
  ASSERT_EQ(doc["books"][0]["keys"].size(), 1u);
  EXPECT_STREQ(doc["books"][0]["keys"][0]["dt"], "2020-01-01 00:00:00");
  ASSERT_EQ(doc["books"][0]["changes"].size(), 1u);
  EXPECT_STREQ(doc["books"][0]["changes"][0]["pos"], "/a");
  EXPECT_EQ(doc["books"][0]["changes"][0]["pageno"].as<int>(), 5);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksRejectsAnEmptyInstallationId) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":[],"delete":[]}}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, "", "X3", nullptr, 0, false, nullptr, 0, outUnmatched, incoming,
                                          outMorePending),
            BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksOmitsPagenoWhenZero) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":[],"delete":[]}}]})"}});
  BookOrbitBookmark changes[1];
  snprintf(changes[0].datetime, sizeof(changes[0].datetime), "2020-01-01 00:00:00");
  changes[0].pos = "/a";
  changes[0].pageno = 0;

  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, changes, 1, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_TRUE(doc["books"][0]["changes"][0]["pageno"].isNull());
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksParsesIncomingAddsAndDeletes) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","more":false,"toApply":{
                     "add":[{"serverId":1,"pos":"/x","title":"Ch1","pageno":3}],
                     "delete":[{"serverId":2,"key":"abc123"}]
                   }}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_FALSE(outUnmatched);
  EXPECT_FALSE(outMorePending);
  ASSERT_EQ(incoming.size(), 2u);

  const auto* add = &incoming[0];
  const auto* del = &incoming[1];
  if (add->deleted) std::swap(add, del);
  EXPECT_EQ(add->serverId, 1u);
  EXPECT_FALSE(add->deleted);
  EXPECT_EQ(add->pos, "/x");
  EXPECT_EQ(add->title, "Ch1");
  EXPECT_EQ(add->pageno, 3);
  EXPECT_EQ(del->serverId, 2u);
  EXPECT_TRUE(del->deleted);
  EXPECT_EQ(del->key, "abc123");
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksSkipsAnAddMissingAPositionAndADeleteMissingAKey) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{
                     "add":[{"serverId":1,"pos":""},{"serverId":2}],
                     "delete":[{"serverId":3},{"serverId":4,"key":""}]
                   }}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(incoming.empty()) << "an entry missing what its own kind requires must never surface";
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksIgnoresAZeroServerId) {
  Rig rig(
      {{200,
        R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":[{"serverId":0,"pos":"/x"}],"delete":[]}}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(incoming.empty()) << "an id of 0 can never be acknowledged, so it must never surface";
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksDropsADuplicateServerIdWithinOneResponse) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{
                     "add":[{"serverId":1,"pos":"/x"},{"serverId":1,"pos":"/x"}],"delete":[]
                   }}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_EQ(incoming.size(), 1u);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksCapsIncomingAtTheBatchLimit) {
  std::string addArray = "[";
  for (size_t i = 0; i < BOOKORBIT_BOOKMARK_BATCH + 5; i++) {
    if (i) addArray += ",";
    addArray += "{\"serverId\":" + std::to_string(i + 1) + ",\"pos\":\"/p" + std::to_string(i) + "\"}";
  }
  addArray += "]";
  const std::string body =
      R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":)" + addArray + R"(,"delete":[]}}]})";
  Rig rig({{200, body}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_EQ(incoming.size(), BOOKORBIT_BOOKMARK_BATCH);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksTruncatesAnOverlongTitleAtAUtf8Boundary) {
  const std::string longTitle(BOOKORBIT_BOOKMARK_TITLE_MAX_CHARS + 50, 'x');
  const std::string body =
      R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","toApply":{"add":[{"serverId":1,"pos":"/x","title":")" +
      longTitle + R"("}],"delete":[]}}]})";
  Rig rig({{200, body}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  ASSERT_EQ(incoming.size(), 1u);
  EXPECT_LE(incoming[0].title.size(), BOOKORBIT_BOOKMARK_TITLE_MAX_CHARS);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksSetsUnmatchedWhenServerDoesNotKnowTheDocument) {
  Rig rig({{200, std::string(R"({"unmatched":[")") + DOC_HASH + R"("],"results":[]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(outUnmatched);
  EXPECT_TRUE(incoming.empty());
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksRejectsAnEmptyResultsArrayAsAmbiguousRatherThanOk) {
  // Regression: `results[0]` used to be trusted positionally. An empty `results` array with no
  // `unmatched` entry either used to default every field emptily and return OK -- which the
  // caller (BookOrbitBookmarkSync::sync()) reads as "confirmed", permanently marking this round's
  // outgoing bookmarks synced even though the server never actually said anything about them.
  Rig rig({{200, R"({"results":[]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksRejectsAResultForADifferentHashRatherThanTrustingItPositionally) {
  // Regression: a `results[0]` entry that answers for some OTHER book (a batching mismatch, a
  // proxy quirk) used to be accepted anyway, since only its position was checked, never its hash.
  Rig rig({{200, R"({"results":[{"hash":"ffffffffffffffffffffffffffffffff","toApply":{"add":[],"delete":[]}}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksFindsTheMatchingResultEvenWhenNotFirst) {
  // The server's own contract allows more than one book per exchange request even though this
  // firmware's client only ever sends one -- the match must be found by hash, not assumed to
  // always be at index 0.
  Rig rig({{200, R"({"results":[
                     {"hash":"ffffffffffffffffffffffffffffffff","toApply":{"add":[],"delete":[]}},
                     {"hash":"0123456789abcdef0123456789abcdef","more":true,"toApply":{"add":[],"delete":[]}}
                   ]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(outMorePending) << "must have read the SECOND entry (matching our hash), not the first";
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksSetsMorePendingFromTheMoreFlag) {
  Rig rig(
      {{200,
        R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","more":true,"toApply":{"add":[],"delete":[]}}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(outMorePending);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksSetsMorePendingFromSkippedNoPosition) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","more":false,"skippedNoPosition":2,)"
                 R"("toApply":{"add":[],"delete":[]}}]})"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  ASSERT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(outMorePending);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksRejectsUnparseableBody) {
  Rig rig({{200, "not json"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::JSON_ERROR);
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksBodyIsBoundedRegardlessOfServerSize) {
  // A server padding its response far past a realistic BOOKORBIT_BOOKMARK_BATCH-sized reply must
  // be truncated rather than grow this device's heap without limit -- an incomplete/truncated
  // JSON document is then simply a parse failure.
  std::string oversized = R"({"results":[{"toApply":{"add":[)";
  for (int i = 0; i < 2000; i++) {
    if (i) oversized += ",";
    oversized += R"({"serverId":1,"pos":"/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
  }
  oversized += R"(],"delete":[]}}]})";
  Rig rig({{200, oversized}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::JSON_ERROR)
      << "an oversized body must be truncated, not parsed in full";
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksMapsHttpFailures) {
  const std::vector<std::pair<int, BookOrbitSyncClient::Error>> cases = {
      {-1, BookOrbitSyncClient::NETWORK_ERROR},
      {401, BookOrbitSyncClient::AUTH_FAILED},
      {500, BookOrbitSyncClient::SERVER_ERROR},
      {400, BookOrbitSyncClient::BAD_REQUEST},
  };
  for (const auto& [code, expected] : cases) {
    Rig rig({{code, ""}});
    bool outUnmatched = false;
    std::vector<BookOrbitIncomingBookmark> incoming;
    bool outMorePending = false;
    EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                            incoming, outMorePending),
              expected)
        << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksNoCredentialsMeansNoRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, "{}"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, ExchangeBookmarksRefusesAHashThatIsNotThirtyTwoHexCharacters) {
  Rig rig({{200, "{}"}});
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  EXPECT_EQ(rig.session.exchangeBookmarks("tooshort", DEVICE_ID, "X3", nullptr, 0, false, nullptr, 0, outUnmatched,
                                          incoming, outMorePending),
            BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

// ---- Bookmark acknowledgment -----------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, AckBookmarksSendsAppliedAndDeletedEntries) {
  Rig rig({{200, ""}});
  BookOrbitBookmarkAck applied;
  applied.serverId = 1;
  applied.failed = false;
  applied.key = "abc123";
  snprintf(applied.datetime, sizeof(applied.datetime), "2020-01-01 00:00:00");
  applied.pos = "/a";
  ASSERT_EQ(rig.session.ackBookmarks(DOC_HASH, DEVICE_ID, "X3", {applied}, {7}), BookOrbitSyncClient::OK);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].url, "https://books.example.test/api/v1/koreader/plugin/bookmarks/exchange-ack");

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_STREQ(doc["deviceId"], DEVICE_ID)
      << "must be the per-installation unique id, not BookOrbitLimits::DEVICE_ID's shared literal";
  ASSERT_EQ(doc["books"][0]["applied"].size(), 1u);
  EXPECT_EQ(doc["books"][0]["applied"][0]["serverId"].as<uint32_t>(), 1u);
  EXPECT_STREQ(doc["books"][0]["applied"][0]["status"], "applied");
  EXPECT_STREQ(doc["books"][0]["applied"][0]["key"], "abc123");
  EXPECT_STREQ(doc["books"][0]["applied"][0]["pos"], "/a");
  ASSERT_EQ(doc["books"][0]["deleted"].size(), 1u);
  EXPECT_EQ(doc["books"][0]["deleted"][0]["serverId"].as<uint32_t>(), 7u);
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksOmitsTheIdentityTripletForAFailedEntry) {
  Rig rig({{200, ""}});
  BookOrbitBookmarkAck failed;
  failed.serverId = 1;
  failed.failed = true;
  ASSERT_EQ(rig.session.ackBookmarks(DOC_HASH, DEVICE_ID, "X3", {failed}, {}), BookOrbitSyncClient::OK);

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_STREQ(doc["books"][0]["applied"][0]["status"], "failed");
  EXPECT_TRUE(doc["books"][0]["applied"][0]["key"].isNull());
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksNoOpWhenBothListsAreEmpty) {
  Rig rig({{200, ""}});
  EXPECT_EQ(rig.session.ackBookmarks(DOC_HASH, DEVICE_ID, "X3", {}, {}), BookOrbitSyncClient::OK);
  EXPECT_TRUE(rig.fake->requests.empty()) << "nothing to acknowledge must never reach the wire";
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksNoCredentialsMeansNoRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, ""}});
  BookOrbitBookmarkAck applied;
  applied.serverId = 1;
  EXPECT_EQ(rig.session.ackBookmarks(DOC_HASH, DEVICE_ID, "X3", {applied}, {}), BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksRefusesAHashThatIsNotThirtyTwoHexCharacters) {
  Rig rig({{200, ""}});
  BookOrbitBookmarkAck applied;
  applied.serverId = 1;
  EXPECT_EQ(rig.session.ackBookmarks("tooshort", DEVICE_ID, "X3", {applied}, {}), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksRejectsAnEmptyInstallationId) {
  Rig rig({{200, ""}});
  BookOrbitBookmarkAck applied;
  applied.serverId = 1;
  EXPECT_EQ(rig.session.ackBookmarks(DOC_HASH, "", "X3", {applied}, {}), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, AckBookmarksMapsHttpFailures) {
  const std::vector<std::pair<int, BookOrbitSyncClient::Error>> cases = {
      {-1, BookOrbitSyncClient::NETWORK_ERROR},
      {401, BookOrbitSyncClient::AUTH_FAILED},
      {500, BookOrbitSyncClient::SERVER_ERROR},
  };
  for (const auto& [code, expected] : cases) {
    Rig rig({{code, ""}});
    BookOrbitBookmarkAck applied;
    applied.serverId = 1;
    EXPECT_EQ(rig.session.ackBookmarks(DOC_HASH, DEVICE_ID, "X3", {applied}, {}), expected) << "code " << code;
  }
}

// ---- Upload payload ------------------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, PayloadMatchesTheServersPageStatsDto) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":2,"duplicates":0,
                     "watermark":1893456060}],"unmatched":[]})"}});
  const auto events = sampleEvents(2);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "Xteink X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);

  ASSERT_EQ(rig.fake->requests.size(), 1u);
  EXPECT_EQ(rig.fake->requests[0].method, "POST");
  EXPECT_EQ(rig.fake->requests[0].url, "https://books.example.test/api/v1/koreader/plugin/page-stats");

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_STREQ(doc["deviceId"], DEVICE_ID)
      << "stats must carry the PER-DEVICE id, not the generic-KOSync shared constant";
  EXPECT_STREQ(doc["deviceModel"], "Xteink X3");
  EXPECT_STREQ(doc["pluginVersion"], BookOrbitLimits::PLUGIN_VERSION);

  ASSERT_EQ(doc["books"].size(), 1u) << "one book per request; the server caps books at 50";
  EXPECT_STREQ(doc["books"][0]["hash"], DOC_HASH);
  ASSERT_EQ(doc["books"][0]["events"].size(), 2u);
  EXPECT_EQ(doc["books"][0]["events"][0]["page"].as<uint32_t>(), 1000u);
  EXPECT_EQ(doc["books"][0]["events"][0]["startTime"].as<uint32_t>(), 1893456000u);
  EXPECT_EQ(doc["books"][0]["events"][0]["durationSeconds"].as<uint32_t>(), 30u);
  EXPECT_EQ(doc["books"][0]["events"][0]["totalPages"].as<uint32_t>(), 10000u);
}

TEST_F(BookOrbitClientProtocolTest, OverlongDeviceModelIsTruncatedRatherThanRejectedByTheServer) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":1,"duplicates":0}],
                    "unmatched":[]})"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, std::string(250, 'x'), events.data(), 1, ack),
            BookOrbitSyncClient::OK);

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_EQ(std::string(doc["deviceModel"].as<const char*>()).size(), BookOrbitLimits::MAX_DEVICE_MODEL_CHARS);
}

TEST_F(BookOrbitClientProtocolTest, DeviceTimeIsOmitted) {
  Rig rig({{200, R"({"results":[],"unmatched":[]})"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack), BookOrbitSyncClient::OK);

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, rig.fake->requests[0].payload));
  EXPECT_TRUE(doc["deviceTime"].isNull());
}

TEST_F(BookOrbitClientProtocolTest, RefusesLocallyRatherThanSendingAnOverlongBatch) {
  Rig rig({{200, "{}"}});
  std::vector<BookOrbitStatEvent> tooMany(BookOrbitLimits::MAX_EVENTS_PER_REQUEST + 1);
  for (auto& e : tooMany) {
    e.startTime = 1893456000u;
    e.durationSeconds = 10;
    e.page = 1;
    e.totalPages = 10000;
  }
  BookOrbitStatsAck ack;
  EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", tooMany.data(), tooMany.size(), ack),
            BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty()) << "an over-cap batch must never reach the wire";
}

TEST_F(BookOrbitClientProtocolTest, RefusesAHashThatIsNotThirtyTwoHexCharacters) {
  Rig rig({{200, "{}"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  EXPECT_EQ(rig.session.uploadPageStats("tooshort", DEVICE_ID, "X3", events.data(), 1, ack),
            BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, RejectsAnEmptyInstallationIdForANonemptyStatsBatch) {
  Rig rig({{200, "{}"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, "", "X3", events.data(), 1, ack), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, EmptyBatchSucceedsWithoutTouchingTheNetwork) {
  Rig rig({{200, "{}"}});
  BookOrbitStatsAck ack;
  EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", nullptr, 0, ack), BookOrbitSyncClient::OK);
  EXPECT_TRUE(rig.fake->requests.empty());
}

TEST_F(BookOrbitClientProtocolTest, NoCredentialsMeansNoUploadRequestAtAll) {
  BOOKORBIT_STORE.setTestCredentials(false);
  Rig rig({{200, "{}"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack),
            BookOrbitSyncClient::NO_CREDENTIALS);
  EXPECT_TRUE(rig.fake->requests.empty());
}

// ---- Response semantics ----------------------------------------------------------------------

TEST_F(BookOrbitClientProtocolTest, EverySuccessStatusInThe2xxRangeCounts) {
  for (const int code : {200, 201, 202, 204, 226, 299}) {
    Rig rig({{code, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":1,"duplicates":0}],
                       "unmatched":[]})"}});
    const auto events = sampleEvents(1);
    BookOrbitStatsAck ack;
    EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack), BookOrbitSyncClient::OK)
        << "code " << code;
    EXPECT_TRUE(ack.matched) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, AcceptedAndDuplicateCountsAreReportedBack) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789ABCDEF0123456789ABCDEF","accepted":5,"duplicates":3,
                     "watermark":99}],"unmatched":[]})"}});
  const auto events = sampleEvents(2);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(ack.matched) << "hash comparison must be case-insensitive";
  EXPECT_FALSE(ack.unmatched);
  EXPECT_EQ(ack.accepted, 5u);
  EXPECT_EQ(ack.duplicates, 3u);
}

TEST_F(BookOrbitClientProtocolTest, AnUnmatchedBookIsReportedSoTheQueueIsRetainedNotDropped) {
  Rig rig({{200, R"({"results":[],"unmatched":["0123456789abcdef0123456789abcdef"]})"}});
  const auto events = sampleEvents(2);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);
  EXPECT_FALSE(ack.matched) << "unmatched means nothing was ingested; the events must survive";
  EXPECT_TRUE(ack.unmatched);
}

TEST_F(BookOrbitClientProtocolTest, A2xxWithAnUnreadableBodyDoesNotConfirmIngestion) {
  Rig rig({{200, "<html>proxy said ok</html>"}});
  const auto events = sampleEvents(3);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);
  EXPECT_FALSE(ack.matched) << "an unparseable body must never license deleting the queue";
  EXPECT_FALSE(ack.unmatched);
  EXPECT_EQ(ack.accepted, 0u);
}

TEST_F(BookOrbitClientProtocolTest, A2xxThatMentionsNeitherListDoesNotConfirmIngestion) {
  Rig rig({{200, R"({"results":[{"hash":"ffffffffffffffffffffffffffffffff","accepted":1}],"unmatched":[]})"}});
  const auto events = sampleEvents(1);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack), BookOrbitSyncClient::OK);
  EXPECT_FALSE(ack.matched) << "a response silent about our hash confirms nothing about it";
  EXPECT_FALSE(ack.unmatched);
}

TEST_F(BookOrbitClientProtocolTest, MissingEndpointIsSoftUnsupportedRatherThanAnError) {
  for (const int code : {404, 405, 501}) {
    Rig rig({{code, ""}});
    const auto events = sampleEvents(1);
    BookOrbitStatsAck ack;
    EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack),
              BookOrbitSyncClient::UNSUPPORTED)
        << "code " << code;
    EXPECT_FALSE(ack.matched) << "code " << code;
  }
}

TEST_F(BookOrbitClientProtocolTest, AHashEchoWithoutFullCountsIsNotConfirmation) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":1,"duplicates":0}],
                    "unmatched":[]})"}});
  const auto events = sampleEvents(3);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);
  EXPECT_FALSE(ack.matched) << "1 of 3 acknowledged must not license deleting 3";
}

TEST_F(BookOrbitClientProtocolTest, MissingOrMistypedCountsAreTreatedAsUnconfirmed) {
  for (const char* body : {R"({"results":[{"hash":"0123456789abcdef0123456789abcdef"}],"unmatched":[]})",
                           R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":"two"}],
                               "unmatched":[]})"}) {
    Rig rig({{200, body}});
    const auto events = sampleEvents(2);
    BookOrbitStatsAck ack;
    ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
              BookOrbitSyncClient::OK);
    EXPECT_FALSE(ack.matched) << "absent counts read as zero, which must fail the check: " << body;
  }
}

TEST_F(BookOrbitClientProtocolTest, DuplicatesCountTowardsAConfirmedBatch) {
  Rig rig({{200, R"({"results":[{"hash":"0123456789abcdef0123456789abcdef","accepted":0,"duplicates":2}],
                    "unmatched":[]})"}});
  const auto events = sampleEvents(2);
  BookOrbitStatsAck ack;
  ASSERT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), events.size(), ack),
            BookOrbitSyncClient::OK);
  EXPECT_TRUE(ack.matched) << "a retry the server deduplicates is still stored";
}

TEST_F(BookOrbitClientProtocolTest, ARejectedPayloadIsDistinguishedFromARetryableFailure) {
  for (const int code : {400, 413, 422}) {
    Rig rig({{code, ""}});
    const auto events = sampleEvents(1);
    BookOrbitStatsAck ack;
    EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack),
              BookOrbitSyncClient::BAD_REQUEST)
        << "code " << code;
  }
  for (const auto& [code, expected] :
       std::vector<std::pair<int, BookOrbitSyncClient::Error>>{{401, BookOrbitSyncClient::AUTH_FAILED},
                                                               {500, BookOrbitSyncClient::SERVER_ERROR},
                                                               {502, BookOrbitSyncClient::SERVER_ERROR},
                                                               {-1, BookOrbitSyncClient::NETWORK_ERROR}}) {
    Rig rig({{code, ""}});
    const auto events = sampleEvents(1);
    BookOrbitStatsAck ack;
    EXPECT_EQ(rig.session.uploadPageStats(DOC_HASH, DEVICE_ID, "X3", events.data(), 1, ack), expected)
        << "code " << code;
    EXPECT_FALSE(ack.matched) << "code " << code;
  }
}

}  // namespace

TEST_F(BookOrbitClientProtocolTest, MalformedUrlIsPermanentWithoutSendingARequest) {
  auto* fake = new FakeKOSyncTransport({});
  fake->beginSucceeds = false;
  BookOrbitSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  BookOrbitProgress progress;
  progress.document = "0123456789abcdef0123456789abcdef";
  progress.progress = "/body/DocFragment[1]/body/p[1]";
  progress.percentage = 0.5f;
  EXPECT_EQ(session.getProgress(progress.document, progress), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_EQ(session.updateProgress(progress), BookOrbitSyncClient::BAD_REQUEST);
  BookOrbitSyncClient::Capabilities caps;
  EXPECT_EQ(session.probeCapabilities(caps), BookOrbitSyncClient::BAD_REQUEST);
  EXPECT_TRUE(fake->requests.empty());
}
