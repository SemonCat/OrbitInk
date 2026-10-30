// Protocol round-trip tests for the REAL lib/KOReaderSync/KOReaderSyncClient.cpp (compiled
// directly, not reimplemented) against a fake, in-memory KOSyncTransport -- no real network,
// WiFi, or TLS/wolfSSL stack. Covers: remote-missing -> PUT-worthy NOT_FOUND, remote-newer ->
// GET/apply data flow, 200/202 success, 401/server/network failure NOT recording success,
// and that one Session reuses a single transport instance across multiple requests instead of
// constructing a fresh one per call (the architectural basis for TLS connection reuse: the real
// freeink::SecureHttpClient reuses its underlying connection whenever the SAME instance receives
// multiple begin()/sendRequest() calls to the same scheme://host:port -- see
// SecureHttpClientTransport.h and KOReaderSyncClient.h's Session doc comment).
//
// NOT covered here (documented gap, not silently skipped): the primary/alternate-hash SMART
// SYNC SELECTION algorithm itself lives in KOReaderSyncActivity::performSync(), which pulls in
// the full Activity/GfxRenderer/Epub/UI stack -- impractical to host-test. What IS covered is
// the infrastructure that algorithm depends on: Session::getProgress()'s hashKind parameter and
// its ability to make multiple independent requests through one reused transport.

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "KOSyncTransport.h"
#include "Logging.h"

namespace {

// Records every begin()/sendRequest() call it receives and returns pre-scripted responses in
// order. A single instance is handed to one Session for its whole lifetime (via the
// unique_ptr-injecting Session constructor), so asserting on ITS call log directly proves
// whether that Session issued one request per transport instance (no reuse) or multiple
// requests through the SAME instance (reuse-capable architecture).
class FakeKOSyncTransport final : public KOSyncTransport {
 public:
  struct ScriptedResponse {
    ScriptedResponse(const int httpCode, std::string body, std::string contentType = "", const size_t chunkSize = 0)
        : httpCode(httpCode), body(std::move(body)), contentType(std::move(contentType)), chunkSize(chunkSize) {}

    int httpCode;
    std::string body;
    std::string contentType;
    size_t chunkSize = 0;
  };

  struct RecordedRequest {
    std::string url;
    std::string method;
    std::string payload;
  };

  // externalEndCallCount, if provided, is a heap cell OWNED BY THE TEST (outlives this object)
  // that end() also increments -- lets a test observe end()-call-count AFTER Session (and thus
  // this fake, which Session's unique_ptr deletes) has already been destroyed, without reading
  // through a dangling `this` after that deletion.
  explicit FakeKOSyncTransport(std::vector<ScriptedResponse> responses,
                               std::shared_ptr<int> externalEndCallCount = nullptr)
      : responses_(std::move(responses)), externalEndCallCount_(std::move(externalEndCallCount)) {}

  bool beginSucceeds = true;

  bool begin(const std::string& url) override {
    lastUrl_ = url;
    return beginSucceeds && !url.empty();
  }

  void addHeader(const std::string& /*name*/, const std::string& /*value*/) override {}

  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                  const AbortCallback& /*shouldAbort*/) override {
    RecordedRequest req;
    req.url = lastUrl_;
    req.method = method;
    req.payload = payload ? std::string(reinterpret_cast<const char*>(payload), payloadLen) : "";
    requests.push_back(req);

    if (nextResponse_ >= responses_.size()) return -1;
    const ScriptedResponse& resp = responses_[nextResponse_++];
    lastBody_ = resp.body;
    lastContentType_ = resp.contentType;
    if (onData && !resp.body.empty()) {
      const size_t chunkSize = resp.chunkSize > 0 ? resp.chunkSize : resp.body.size();
      for (size_t offset = 0; offset < resp.body.size(); offset += chunkSize) {
        const size_t len = std::min(chunkSize, resp.body.size() - offset);
        offeredBodyBytes += len;
        if (!onData(reinterpret_cast<const uint8_t*>(resp.body.data() + offset), len)) {
          callbackStopped = true;
          break;
        }
        acceptedBodyBytes += len;
      }
    }
    return resp.httpCode;
  }

  std::string getResponseHeader(const std::string& name) const override {
    ++responseHeaderLookups;
    return name == "Content-Type" ? lastContentType_ : "";
  }
  const std::string& getString() const override { return lastBody_; }
  void end() override {
    endCallCount++;
    if (externalEndCallCount_) (*externalEndCallCount_)++;
  }
  bool reuses() const override { return true; }

  std::vector<RecordedRequest> requests;
  int endCallCount = 0;
  size_t offeredBodyBytes = 0;
  size_t acceptedBodyBytes = 0;
  bool callbackStopped = false;
  mutable int responseHeaderLookups = 0;

 private:
  std::vector<ScriptedResponse> responses_;
  size_t nextResponse_ = 0;
  std::string lastUrl_;
  std::string lastBody_;
  std::string lastContentType_;
  std::shared_ptr<int> externalEndCallCount_;
};

class KOSyncProtocolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    KOReaderCredentialStore::getInstance().resetToDefaults();
    CapturedLogs::clear();
  }
};

}  // namespace

// ---- Remote missing -> PUT-worthy NOT_FOUND --------------------------------------------------

TEST_F(KOSyncProtocolTest, RemoteMissingReturnsNotFound) {
  auto* fake = new FakeKOSyncTransport({{404, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress progress;
  const auto result = session.getProgress("somehash", progress);

  EXPECT_EQ(result, KOReaderSyncClient::NOT_FOUND);
  EXPECT_EQ(session.lastHttpCode(), 404);
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_EQ(fake->requests[0].method, "GET");
}

// ---- Remote newer -> GET returns applicable progress data ------------------------------------

TEST_F(KOSyncProtocolTest, RemoteNewerProgressIsParsedCorrectly) {
  const std::string body =
      R"({"progress":"/body/DocFragment[3]/body/p[5]","percentage":0.42,"device":"KOReader","device_id":"kobo","timestamp":1700000000})";
  auto* fake = new FakeKOSyncTransport({{200, body, "Application/Vnd.Koreader.V1+Json; charset=UTF-8"}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress progress;
  const auto result = session.getProgress("somehash", progress);

  EXPECT_EQ(result, KOReaderSyncClient::OK);
  EXPECT_FLOAT_EQ(progress.percentage, 0.42f);
  EXPECT_EQ(progress.progress, "/body/DocFragment[3]/body/p[5]");
  EXPECT_EQ(progress.device, "KOReader");
  EXPECT_FALSE(progress.position.has_value())
      << "rich position must not be parsed for a non-CrossPoint server (usesCrossPointSyncServer=false)";
}

TEST_F(KOSyncProtocolTest, GetProgressRejectsMissingInvalidOrMillisecondTimestamp) {
  for (const char* body : {
           R"({"progress":"/body/p[1]","percentage":0.42})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":0})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":1700000000000})",
           R"({"progress":"/body/p[1]","percentage":0.42,"timestamp":"1700000000"})",
       }) {
    auto* fake = new FakeKOSyncTransport({{200, body}});
    KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
    KOReaderProgress progress;
    EXPECT_EQ(session.getProgress("somehash", progress), KOReaderSyncClient::JSON_ERROR) << body;
  }
}

TEST_F(KOSyncProtocolTest, RichPositionIsParsedOnlyForCrossPointServer) {
  KOReaderCredentialStore::getInstance().setTestUsesCrossPointSyncServer(true);
  const std::string body =
      R"({"progress":"/body/DocFragment[1]/body","percentage":0.1,"device":"d","device_id":"i","timestamp":1700000000,)"
      R"("position":{"pctQ":100000,"spine":2,"page":3,"pages":10,"para":7,"xpath":"/body/p[1]"}})";
  auto* fake = new FakeKOSyncTransport({{200, body}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress progress;
  ASSERT_EQ(session.getProgress("h", progress), KOReaderSyncClient::OK);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_EQ(progress.position->spineIndex, 2);
  EXPECT_EQ(progress.position->pageNumber, 3);
  EXPECT_EQ(progress.position->totalPages, 10);
  ASSERT_TRUE(progress.position->paragraphIndex.has_value());
  EXPECT_EQ(*progress.position->paragraphIndex, 7);
}

// ---- 200/202 success for PUT ------------------------------------------------------------------

TEST_F(KOSyncProtocolTest, UpdateProgressAccepts200) {
  auto* fake = new FakeKOSyncTransport({{200, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress toPush;
  toPush.document = "h";
  toPush.progress = "/body/p[1]";
  toPush.percentage = 0.5f;
  const auto result = session.updateProgress(toPush);

  EXPECT_EQ(result, KOReaderSyncClient::OK);
  ASSERT_EQ(fake->requests.size(), 1u);
  EXPECT_EQ(fake->requests[0].method, "PUT");
  // The outgoing JSON body must actually carry the document/progress/percentage fields.
  EXPECT_NE(fake->requests[0].payload.find("\"document\":\"h\""), std::string::npos);
  EXPECT_NE(fake->requests[0].payload.find("/body/p[1]"), std::string::npos);
}

TEST_F(KOSyncProtocolTest, UpdateProgressAccepts202) {
  auto* fake = new FakeKOSyncTransport({{202, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress toPush;
  toPush.document = "h";
  EXPECT_EQ(session.updateProgress(toPush), KOReaderSyncClient::OK);
}

// ---- All-2xx acceptance across every request kind (0423276d2cdb) -------------------------------

TEST_F(KOSyncProtocolTest, UpdateProgressAcceptsArbitrary2xx) {
  // 226 (IM Used) is a real, if obscure, 2xx status -- picked specifically because it is NOT one
  // of the two codes (200/202) this categorization used to special-case.
  auto* fake = new FakeKOSyncTransport({{226, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress toPush;
  toPush.document = "h";
  EXPECT_EQ(session.updateProgress(toPush), KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, AuthenticateAcceptsArbitrary2xx) {
  auto* fake = new FakeKOSyncTransport({{204, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  EXPECT_EQ(session.authenticate(), KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, CreateUserAcceptsArbitrary2xxAndStill402sOnUserExists) {
  auto* fakeOk = new FakeKOSyncTransport({{201, ""}});
  KOReaderSyncClient::Session sessionOk{std::unique_ptr<KOSyncTransport>(fakeOk)};
  EXPECT_EQ(sessionOk.createUser(), KOReaderSyncClient::OK);

  auto* fakeExists = new FakeKOSyncTransport({{402, ""}});
  KOReaderSyncClient::Session sessionExists{std::unique_ptr<KOSyncTransport>(fakeExists)};
  EXPECT_EQ(sessionExists.createUser(), KOReaderSyncClient::USER_EXISTS)
      << "402 must still be distinguished from a generic 2xx success";
}

TEST_F(KOSyncProtocolTest, GetProgressAcceptsA2xxOtherThan200AndParsesTheBody) {
  // Some servers may answer a successful GET with 201/226/etc. instead of 200; the body must
  // still be parsed exactly like a 200 would be.
  const std::string body = R"({"progress":"/body/p[2]","percentage":0.33,"device":"d","device_id":"i","timestamp":1700000000})";
  auto* fake = new FakeKOSyncTransport({{226, body}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  ASSERT_EQ(session.getProgress("h", progress), KOReaderSyncClient::OK);
  EXPECT_FLOAT_EQ(progress.percentage, 0.33f);
  EXPECT_EQ(progress.progress, "/body/p[2]");
}

// ---- GET 204: No Content maps to NOT_FOUND and must never reach the JSON parser ----------------

TEST_F(KOSyncProtocolTest, GetProgress204MapsToNotFoundWithoutParsingAnyBody) {
  // The scripted body, if it WERE parsed, would deserialize into a valid, non-default
  // KOReaderProgress (nonzero percentage, a real device name) -- so asserting NOT_FOUND alone
  // would not prove the parser was skipped (a 204 that parsed this body and then happened to map
  // to NOT_FOUND would pass that alone too). Asserting the output is untouched is the
  // discriminating check: a real parse would have overwritten these fields.
  const std::string wouldParseToRealData =
      R"({"progress":"/body/DocFragment[9]/body/p[1]","percentage":0.77,"device":"OtherDevice",)"
      R"("device_id":"other","timestamp":1700000000})";
  auto* fake = new FakeKOSyncTransport({{204, wouldParseToRealData}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  // Value-initialized (not default-constructed): KOReaderProgress::percentage has no default
  // member initializer, so a plain `KOReaderProgress progress;` leaves it uninitialized garbage,
  // which would make the post-call zero-check below meaningless.
  KOReaderProgress progress{};
  const auto result = session.getProgress("h", progress);

  EXPECT_EQ(result, KOReaderSyncClient::NOT_FOUND);
  EXPECT_EQ(session.lastHttpCode(), 204);
  EXPECT_EQ(progress.percentage, 0.0f) << "204 must not have been parsed into outProgress";
  EXPECT_TRUE(progress.progress.empty()) << "204 must not have been parsed into outProgress";
  EXPECT_TRUE(progress.device.empty()) << "204 must not have been parsed into outProgress";
}

TEST_F(KOSyncProtocolTest, GetProgress204WithEmptyBodyAlsoMapsToNotFound) {
  auto* fake = new FakeKOSyncTransport({{204, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  EXPECT_EQ(session.getProgress("h", progress), KOReaderSyncClient::NOT_FOUND);
}

// ---- Failure categorization: 401 / server / network -- none of these are "success" -----------

TEST_F(KOSyncProtocolTest, Http401IsAuthFailedNotSuccess) {
  auto* fake = new FakeKOSyncTransport({{401, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress toPush;
  const auto result = session.updateProgress(toPush);
  EXPECT_EQ(result, KOReaderSyncClient::AUTH_FAILED);
  EXPECT_NE(result, KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, Http500IsServerErrorNotSuccess) {
  auto* fake = new FakeKOSyncTransport({{500, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress toPush;
  const auto result = session.updateProgress(toPush);
  EXPECT_EQ(result, KOReaderSyncClient::SERVER_ERROR);
  EXPECT_NE(result, KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, NegativeHttpCodeIsNetworkErrorNotSuccess) {
  // A negative return from sendRequest models a transport-level failure (no response at all --
  // connection refused, TLS handshake failure, timeout).
  auto* fake = new FakeKOSyncTransport({{-1, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress toPush;
  const auto result = session.updateProgress(toPush);
  EXPECT_EQ(result, KOReaderSyncClient::NETWORK_ERROR);
  EXPECT_NE(result, KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, MalformedJsonBodyIsJsonErrorNotSuccess) {
  auto* fake = new FakeKOSyncTransport({{200, "{not valid json"}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  const auto result = session.getProgress("h", progress);
  EXPECT_EQ(result, KOReaderSyncClient::JSON_ERROR);
  EXPECT_NE(result, KOReaderSyncClient::OK);
}

TEST_F(KOSyncProtocolTest, OversizedProgressBodyStopsAtTheStrictJsonLimit) {
  constexpr size_t EXPECTED_BODY_LIMIT = 4096;
  std::string body = R"({"pad":")";
  body += std::string(EXPECTED_BODY_LIMIT * 2, 'x');
  body += R"("})";
  auto* fake = new FakeKOSyncTransport({{200, body, "application/json", 512}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress progress{};
  EXPECT_EQ(session.getProgress("h", progress), KOReaderSyncClient::JSON_ERROR);
  EXPECT_TRUE(fake->callbackStopped) << "the transport must be stopped instead of draining an unbounded body";
  EXPECT_LE(fake->acceptedBodyBytes, EXPECTED_BODY_LIMIT);
  EXPECT_LT(fake->offeredBodyBytes, body.size());
}

TEST_F(KOSyncProtocolTest, HtmlProgressResponseIsRejectedBeforeItsBodyIsAccumulated) {
  const std::string body =
      R"(<!doctype html><html><head><link rel="modulepreload" href="/assets/index.js"></head></html>)";
  auto* fake = new FakeKOSyncTransport({{200, body, "text/html; charset=utf-8", 32}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress progress{};
  EXPECT_EQ(session.getProgress("h", progress), KOReaderSyncClient::JSON_ERROR);
  EXPECT_GT(fake->responseHeaderLookups, 0) << "an available Content-Type must be checked before JSON parsing";
  EXPECT_TRUE(fake->callbackStopped);
  EXPECT_EQ(fake->acceptedBodyBytes, 0u) << "known HTML must not be accumulated";
}

// ---- Gating: no credentials / insufficient heap -- neither should touch the transport ---------

TEST_F(KOSyncProtocolTest, NoCredentialsGatesOutBeforeAnyRequest) {
  KOReaderCredentialStore::getInstance().setTestCredentials(false);
  auto* fake = new FakeKOSyncTransport({{200, "{}"}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  EXPECT_EQ(session.getProgress("h", progress), KOReaderSyncClient::NO_CREDENTIALS);
  EXPECT_EQ(fake->requests.size(), 0u) << "must not touch the transport at all without credentials";
}

// ---- Connection reuse: one Session, one transport instance, multiple requests -----------------

TEST_F(KOSyncProtocolTest, OneSessionReusesTheSameTransportAcrossGetAndPut) {
  // Two scripted responses: the primary GET, then the PUT -- both served by the SAME transport
  // instance, proving the Session routed both requests through one reusable connection object
  // instead of constructing a fresh transport (and thus a fresh TLS handshake in production)
  // per call.
  auto* fake = new FakeKOSyncTransport({{404, ""}, {200, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress remote;
  const auto getResult = session.getProgress("hash1", remote, nullptr, "primary");
  EXPECT_EQ(getResult, KOReaderSyncClient::NOT_FOUND);

  KOReaderProgress toPush;
  toPush.document = "hash1";
  const auto putResult = session.updateProgress(toPush, nullptr, "primary");
  EXPECT_EQ(putResult, KOReaderSyncClient::OK);

  ASSERT_EQ(fake->requests.size(), 2u) << "both requests must reach the same transport instance";
  EXPECT_EQ(fake->requests[0].method, "GET");
  EXPECT_EQ(fake->requests[1].method, "PUT");
  // end() is only called once, at Session teardown -- not between the two requests, which is
  // exactly what lets freeink::SecureHttpClient keep its underlying connection open between them.
  EXPECT_EQ(fake->endCallCount, 0) << "end() must not be called between requests within one session";
}

TEST_F(KOSyncProtocolTest, SessionCallsEndExactlyOnceOnDestruction) {
  // endCallCount lives in a heap cell OWNED BY THE TEST (via shared_ptr), not inside the fake
  // transport object itself -- so it stays valid after `session` (and the unique_ptr-owned fake
  // it deletes) go out of scope. Asserting on `fake->endCallCount` post-destruction would be a
  // use-after-free/UB read of freed memory; this sidesteps that entirely.
  auto endCallCount = std::make_shared<int>(0);
  auto* fake = new FakeKOSyncTransport({{200, "{}"}}, endCallCount);
  {
    KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
    KOReaderProgress progress;
    session.getProgress("h", progress);
    EXPECT_EQ(*endCallCount, 0);
  }
  EXPECT_EQ(*endCallCount, 1) << "Session's destructor must close the connection exactly once, cleanly";
}

TEST_F(KOSyncProtocolTest, PrimaryAndAlternateHashesEachMakeTheirOwnIndependentRequest) {
  // Infrastructure test for smart-sync: Session.getProgress() with different hashKind labels
  // and different document hashes must issue two independent, correctly-routed requests through
  // the same session (the actual primary/alternate SELECTION decision is made by
  // KOReaderSyncActivity::performSync(), not tested here -- see file header).
  auto* fake = new FakeKOSyncTransport({{404, ""},
                                        {200, R"({"progress":"/body/p[9]","percentage":0.9,)"
                                              R"("device":"d","device_id":"i","timestamp":1700000000})"}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};

  KOReaderProgress primary;
  const auto primaryResult = session.getProgress("primary-hash", primary, nullptr, "primary");
  KOReaderProgress alternate;
  const auto altResult = session.getProgress("alternate-hash", alternate, nullptr, "alternate");

  EXPECT_EQ(primaryResult, KOReaderSyncClient::NOT_FOUND);
  EXPECT_EQ(altResult, KOReaderSyncClient::OK);
  EXPECT_FLOAT_EQ(alternate.percentage, 0.9f);
  ASSERT_EQ(fake->requests.size(), 2u);
}

// ---- Release-visible INFO logging: final GET/PUT outcome, no leaked secrets -------------------

TEST_F(KOSyncProtocolTest, GetOutcomeIsLoggedAtInfoLevelWithMethodCodeAndResult) {
  auto* fake = new FakeKOSyncTransport({{404, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  session.getProgress("somehash", progress, nullptr, "primary");

  const auto& lines = CapturedLogs::infoLines();
  const auto it = std::find_if(lines.begin(), lines.end(),
                               [](const std::string& l) { return l.find("GET /syncs/progress") != std::string::npos; });
  ASSERT_NE(it, lines.end()) << "expected an INFO log for the final GET outcome";
  EXPECT_NE(it->find("http=404"), std::string::npos) << "log line: " << *it;
  EXPECT_NE(it->find("primary"), std::string::npos) << "log line: " << *it;
  EXPECT_NE(it->find(KOReaderSyncClient::errorString(KOReaderSyncClient::NOT_FOUND)), std::string::npos)
      << "log line must carry the categorized result, not just the raw HTTP code: " << *it;
}

TEST_F(KOSyncProtocolTest, PutOutcomeIsLoggedAtInfoLevelWithMethodCodeAndResult) {
  auto* fake = new FakeKOSyncTransport({{200, ""}});
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress toPush;
  toPush.document = "h";
  session.updateProgress(toPush, nullptr, "primary");

  const auto& lines = CapturedLogs::infoLines();
  const auto it = std::find_if(lines.begin(), lines.end(),
                               [](const std::string& l) { return l.find("PUT /syncs/progress") != std::string::npos; });
  ASSERT_NE(it, lines.end()) << "expected an INFO log for the final PUT outcome";
  EXPECT_NE(it->find("http=200"), std::string::npos) << "log line: " << *it;
  EXPECT_NE(it->find(KOReaderSyncClient::errorString(KOReaderSyncClient::OK)), std::string::npos)
      << "log line: " << *it;
}

TEST_F(KOSyncProtocolTest, OutcomeLoggingNeverLeaksCredentialsOrFullUrl) {
  KOReaderCredentialStore::getInstance().setTestBaseUrl("https://sync.example.test/secret-path-segment");
  auto* fakeGet = new FakeKOSyncTransport({{401, ""}});
  {
    KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fakeGet)};
    KOReaderProgress progress;
    session.getProgress("h", progress);
  }
  auto* fakePut = new FakeKOSyncTransport({{200, ""}});
  {
    KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fakePut)};
    KOReaderProgress toPush;
    toPush.document = "h";
    session.updateProgress(toPush);
  }

  const auto& lines = CapturedLogs::infoLines();
  ASSERT_FALSE(lines.empty());
  for (const auto& line : lines) {
    EXPECT_EQ(line.find(KOReaderCredentialStore::getInstance().getPassword()), std::string::npos)
        << "log line leaks the plaintext password: " << line;
    EXPECT_EQ(line.find(KOReaderCredentialStore::getInstance().getMd5Password()), std::string::npos)
        << "log line leaks the MD5 password: " << line;
    EXPECT_EQ(line.find("secret-path-segment"), std::string::npos)
        << "log line leaks the full sync server URL: " << line;
    EXPECT_EQ(line.find("Authorization"), std::string::npos) << "log line leaks an auth header: " << line;
    EXPECT_EQ(line.find("Basic "), std::string::npos) << "log line leaks a Basic-auth value: " << line;
  }
}

TEST_F(KOSyncProtocolTest, MalformedUrlIsPermanentWithoutSendingARequest) {
  auto* fake = new FakeKOSyncTransport({});
  fake->beginSucceeds = false;
  KOReaderSyncClient::Session session{std::unique_ptr<KOSyncTransport>(fake)};
  KOReaderProgress progress;
  progress.document = "0123456789abcdef0123456789abcdef";
  progress.progress = "/body/DocFragment[1]/body/p[1]";
  progress.percentage = 0.5f;
  EXPECT_EQ(session.getProgress(progress.document, progress), KOReaderSyncClient::BAD_REQUEST);
  EXPECT_EQ(session.updateProgress(progress), KOReaderSyncClient::BAD_REQUEST);
  EXPECT_TRUE(fake->requests.empty());
}
