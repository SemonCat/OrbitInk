#include <gtest/gtest.h>
#include <HttpDownloader.h>
#include <SecureClient.h>

// Only the socket and device filesystem are replaced; HTTP parsing is the pinned SDK.
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
}  // namespace freeink

namespace {
constexpr char destination[] = "/download.part";
class DownloadCancellation : public testing::Test {
  void SetUp() override { HalStorage::resetTestFiles(); }
};

TEST_F(DownloadCancellation, UnknownLengthReportsBytesAndPollsInput) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
  size_t callbacks = 0;
  size_t downloaded = 0;
  EXPECT_EQ(HttpDownloader::downloadToFile("http://peer/file", destination, [&](size_t bytes, size_t total) {
    ++callbacks;
    downloaded = bytes;
    EXPECT_EQ(total, 0u);
  }), HttpDownloader::OK);
  EXPECT_GE(callbacks, 2u);
  EXPECT_EQ(downloaded, 3u);
  EXPECT_EQ(HalStorage::readTestFile(destination), (std::vector<uint8_t>{'a', 'b', 'c'}));
}

TEST_F(DownloadCancellation, FinalChunkInputCancellationRemovesPartialAndReturnsAborted) {
  for (const auto* response : {
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc",
           "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc"}) {
    HostPeer::reset(response);
    bool cancelled = false;
    EXPECT_EQ(HttpDownloader::downloadToFile("http://peer/file", destination,
        [&](size_t bytes, size_t) { if (bytes > 0) cancelled = true; }, &cancelled), HttpDownloader::ABORTED);
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(Storage.exists(destination));
  }
}

TEST_F(DownloadCancellation, InputPollingAbortsDuringHeaderWait) {
  HostPeer::reset("HTTP/1.1 200 OK\r\nContent-Len", true);
  bool cancelled = false;
  EXPECT_EQ(HttpDownloader::downloadToFile("http://peer/file", destination,
      [&](size_t, size_t) { if (millis() >= 75) cancelled = true; }, &cancelled), HttpDownloader::ABORTED);
  EXPECT_TRUE(cancelled);
  EXPECT_LT(millis(), 150u);
  EXPECT_FALSE(Storage.exists(destination));
}

TEST_F(DownloadCancellation, InputPollingAbortsStalledFixedChunkedAndCloseDelimitedBodies) {
  for (const auto* response : {
           "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab",
           "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nab"}) {
    HostPeer::reset(response, true);
    bool cancelled = false;
    EXPECT_EQ(HttpDownloader::downloadToFile("http://peer/file", destination,
        [&](size_t, size_t) { if (millis() >= 75) cancelled = true; }, &cancelled), HttpDownloader::ABORTED);
    EXPECT_TRUE(cancelled);
    EXPECT_LT(millis(), 150u);
    EXPECT_FALSE(Storage.exists(destination));
  }
}
}  // namespace
