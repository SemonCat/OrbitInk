#include <gtest/gtest.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitSyncClient.h>
#include <SecureHttpClientTransport.h>

// Socket replacement under the actual, unmodified SDK parser. TLS cryptography is not tested.
namespace freeink {
SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress ip, uint16_t port) { return _transport.connect(ip, port); }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t value) { return _transport.write(value); }
size_t SecureClient::write(const uint8_t* bytes, size_t count) { return _transport.write(bytes, count); }
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* bytes, size_t count) { return _transport.read(bytes, count); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() { _transport.flush(); }
void SecureClient::stop() { _transport.stop(); }
uint8_t SecureClient::connected() { return _transport.connected(); }
bool SecureClient::tls13Available() { return false; }
}

namespace {
const auto discard = [](const uint8_t*, size_t) { return true; };

TEST(SecureSyncTransport, SdkReturns200ForTruncatedPutResponse) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{}");
  freeink::SecureHttpClient sdk;
  ASSERT_TRUE(sdk.begin("http://peer/syncs/progress"));
  EXPECT_EQ(sdk.sendRequest("PUT", nullptr, 0, discard), 200);
  EXPECT_FALSE(sdk.responseComplete());
}

TEST(SecureSyncTransport, OwnedAdapterRejectsTruncatedPutResponse) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{}");
  SecureHttpClientTransport transport;
  ASSERT_TRUE(transport.begin("http://peer/syncs/progress"));
  EXPECT_LT(transport.sendRequest("PUT", nullptr, 0, discard, nullptr), 0);
}

TEST(SecureSyncTransport, OwnedAdapterRejectsBodyAbortAfter200) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", true);
  SecureHttpClientTransport transport;
  ASSERT_TRUE(transport.begin("http://peer/syncs/progress"));
  EXPECT_LT(transport.sendRequest("PUT", nullptr, 0, discard, [] {
    return HostPeer::offset >= HostPeer::bodyOffset;
  }), 0);
}

TEST(SecureSyncTransport, OwnedAdapterRejectsBodyCallbackCancellation) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
  SecureHttpClientTransport transport;
  ASSERT_TRUE(transport.begin("http://peer/syncs/progress"));
  EXPECT_LT(transport.sendRequest("GET", nullptr, 0, [](const uint8_t*, size_t) { return false; }, nullptr), 0);
}

TEST(SecureSyncTransport, BufferedRequestStillHonorsAbort) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", true);
  SecureHttpClientTransport transport;
  ASSERT_TRUE(transport.begin("http://peer/syncs/progress"));
  EXPECT_LT(transport.sendRequest("PUT", nullptr, 0, nullptr, [] { return true; }), 0);
  EXPECT_TRUE(HostPeer::request.empty());
}

TEST(SecureSyncTransport, CompleteEmptyAndChunkedResponsesKeepTheirStatus) {
  for (const auto* response : {"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
                              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n",
                              "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"}) {
    HostPeer::reset(response);
    SecureHttpClientTransport transport;
    ASSERT_TRUE(transport.begin("http://peer/syncs/progress"));
    EXPECT_GT(transport.sendRequest("PUT", nullptr, 0, discard, nullptr), 0);
  }
}

TEST(SecureSyncTransport, RealBookOrbitClientDoesNotConfirmTruncatedPut) {
  BOOKORBIT_STORE.resetToDefaults();
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{}");
  BookOrbitSyncClient::Session session(std::make_unique<SecureHttpClientTransport>());
  BookOrbitProgress progress;
  progress.document = "0123456789abcdef0123456789abcdef";
  progress.progress = "/body/DocFragment[1]/body/p[2]";
  progress.percentage = 0.25f;
  progress.deviceId = "synthetic-device";
  progress.timestamp = 1791072000;
  EXPECT_EQ(session.updateProgress(progress), BookOrbitSyncClient::NETWORK_ERROR);
  EXPECT_LT(session.lastHttpCode(), 0);
  EXPECT_NE(HostPeer::request.find("PUT /api/v1/koreader/syncs/progress"), std::string::npos);
}
}
