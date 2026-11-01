#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#include "lib/FirmwareUpdate/OtaHttp.h"

using ota_http::ResponseHeadParser;
using Status = ResponseHeadParser::Status;

namespace {
size_t feedStr(ResponseHeadParser& p, const std::string& s, size_t chunk = SIZE_MAX) {
  size_t used = 0;
  for (size_t off = 0; off < s.size() && p.status() == Status::NEED_MORE; off += chunk) {
    const size_t n = std::min(chunk, s.size() - off);
    used += p.feed(reinterpret_cast<const uint8_t*>(s.data()) + off, n);
  }
  return used;
}

// Shape of a real github.com release redirect: ~4 KB CSP line and a ~930-char signed Location.
std::string githubRedirect() {
  return "HTTP/1.1 302 Found\r\n"
         "Server: github.com\r\n"
         "Content-Security-Policy: default-src 'none'; " +
         std::string(3950, 'x') +
         "\r\n"
         "Set-Cookie: _gh_sess=" +
         std::string(400, 'c') +
         "\r\n"
         "Location: https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sv=2018" +
         std::string(830, 'q') +
         "\r\n"
         "Content-Length: 0\r\n"
         "\r\n";
}
}  // namespace

TEST(OtaHttp, ParsesRealisticRedirectWithConstantMemory) {
  for (size_t chunk : {1u, 13u, 512u, 100000u}) {
    ResponseHeadParser p;
    const auto head = githubRedirect();
    EXPECT_EQ(feedStr(p, head + "BODY", chunk), head.size()) << chunk;
    ASSERT_EQ(p.status(), Status::DONE) << chunk;
    EXPECT_EQ(p.statusCode(), 302);
    ASSERT_NE(p.location(), nullptr);
    EXPECT_EQ(std::string(p.location()).rfind("https://release-assets.githubusercontent.com/", 0), 0u);
    EXPECT_TRUE(p.hasContentLength());
    EXPECT_EQ(p.contentLength(), 0u);
  }
  // The parser's whole state is a fixed object: no allocation scales with the response.
  EXPECT_LT(sizeof(ResponseHeadParser), 3200u);
}

TEST(OtaHttp, ParsesOkWithLengthAndLeavesBody) {
  ResponseHeadParser p;
  const std::string head = "HTTP/1.1 200 OK\r\ncontent-length: 5853045\r\nX-Other: y\r\n\r\n";
  EXPECT_EQ(feedStr(p, head + "\xE9rest"), head.size());
  EXPECT_EQ(p.status(), Status::DONE);
  EXPECT_EQ(p.statusCode(), 200);
  EXPECT_EQ(p.contentLength(), 5853045u);
  EXPECT_FALSE(p.hasTransferEncoding());
  EXPECT_EQ(p.location(), nullptr);
}

TEST(OtaHttp, FlagsTransferEncoding) {
  ResponseHeadParser p;
  feedStr(p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
  EXPECT_EQ(p.status(), Status::DONE);
  EXPECT_TRUE(p.hasTransferEncoding());
}

TEST(OtaHttp, RejectsMalformedOrHostileHeads) {
  const std::string cases[] = {
      "HTTP/2 200\r\n\r\n",
      "HTTP/1.1 20x OK\r\n\r\n",
      "HTTP/1.1 200OK\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 12a\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
      "HTTP/1.1 302 Found\r\nLocation: /a\r\nLocation: /b\r\n\r\n",
      "HTTP/1.1 302 Found\r\nLocation: https://x/" + std::string(ota_http::LOCATION_MAX + 10, 'a') + "\r\n\r\n",
      "HTTP/1.1 200 OK\r\nX-Folded: a\r\n continued\r\n\r\n",
      "HTTP/1.1 200 OK\r\nno colon here\r\n\r\n",
  };
  for (const auto& c : cases) {
    ResponseHeadParser p;
    feedStr(p, c);
    EXPECT_EQ(p.status(), Status::ERROR) << c.substr(0, 60);
  }
}

TEST(OtaHttp, OverlongIgnoredHeaderIsDiscardedNotFatal) {
  ResponseHeadParser p;
  feedStr(p, "HTTP/1.1 200 OK\r\nX-Huge: " + std::string(20000, 'z') + "\r\nContent-Length: 3\r\n\r\n");
  EXPECT_EQ(p.status(), Status::DONE);
  EXPECT_EQ(p.contentLength(), 3u);
}

TEST(OtaHttp, HeadSizeAndLineCountAreBounded) {
  {
    ResponseHeadParser p;
    feedStr(p, "HTTP/1.1 200 OK\r\nX: " + std::string(ota_http::MAX_HEAD_BYTES, 'z') + "\r\n\r\n");
    EXPECT_EQ(p.status(), Status::ERROR);
  }
  {
    std::string many = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 200; i++) many += "X: y\r\n";
    ResponseHeadParser p;
    feedStr(p, many + "\r\n");
    EXPECT_EQ(p.status(), Status::ERROR);
  }
}

TEST(OtaHttp, UrlParsingAndRedirectResolution) {
  ota_http::Url u;
  ASSERT_TRUE(
      ota_http::parseHttpsUrl("https://github.com/SemonCat/BookOrbit-X3/releases/latest/download/manifest.json", u));
  EXPECT_STREQ(u.host, "github.com");
  EXPECT_EQ(u.port, 443);
  EXPECT_STREQ(u.path, "/SemonCat/BookOrbit-X3/releases/latest/download/manifest.json");
  ASSERT_TRUE(ota_http::parseHttpsUrl("https://github.com", u));
  EXPECT_STREQ(u.path, "/");
  ASSERT_TRUE(ota_http::parseHttpsUrl("https://h.example:8443/p", u));
  EXPECT_EQ(u.port, 8443);

  const char* bad[] = {"http://github.com/",           "https://user@github.com/",  "https:///x",
                       "https://github.com:0/",        "https://github.com:99999/", "https://github.com/a b",
                       "https://github.com/a\r\nX: y", "ftp://github.com/"};
  for (const char* b : bad) EXPECT_FALSE(ota_http::parseHttpsUrl(b, u)) << b;

  ota_http::Url cur;
  ASSERT_TRUE(ota_http::parseHttpsUrl("https://github.com/a/b", cur));
  ota_http::Url next;
  ASSERT_TRUE(ota_http::resolveRedirect(cur, "/SemonCat/BookOrbit-X3/releases/download/x3-b2/manifest.json", next));
  EXPECT_STREQ(next.host, "github.com");
  ASSERT_TRUE(ota_http::resolveRedirect(cur, "https://release-assets.githubusercontent.com/x?y=z", next));
  EXPECT_STREQ(next.host, "release-assets.githubusercontent.com");
  EXPECT_STREQ(next.path, "/x?y=z");
  EXPECT_FALSE(ota_http::resolveRedirect(cur, "http://github.com/x", next));
  EXPECT_FALSE(ota_http::resolveRedirect(cur, "//evil.example/x", next));
  EXPECT_FALSE(ota_http::resolveRedirect(cur, "relative/path", next));
  EXPECT_FALSE(ota_http::resolveRedirect(cur, nullptr, next));
}

TEST(OtaHttp, HostAllowlist) {
  EXPECT_TRUE(ota_http::isAllowedHost("github.com"));
  EXPECT_TRUE(ota_http::isAllowedHost("release-assets.githubusercontent.com"));
  EXPECT_TRUE(ota_http::isAllowedHost("objects.githubusercontent.com"));
  EXPECT_TRUE(ota_http::isAllowedHost("GitHub.com"));
  EXPECT_FALSE(ota_http::isAllowedHost("api.github.com"));
  EXPECT_FALSE(ota_http::isAllowedHost("github.com.evil.example"));
  EXPECT_FALSE(ota_http::isAllowedHost("evilgithub.com"));
  EXPECT_FALSE(ota_http::isAllowedHost(nullptr));
}

TEST(OtaHttp, LegacyRepositoryRenameKeepsSignedFirmwareUrlAndBoundedRedirectsUsable) {
  ota_http::Url original, renamed, asset;
  ASSERT_TRUE(ota_http::parseHttpsUrl(
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-ota-b1/firmware.bin", original));
  ASSERT_TRUE(ota_http::resolveRedirect(
      original, "https://github.com/SemonCat/OrbitInk/releases/download/x3-ota-b1/firmware.bin", renamed));
  ASSERT_TRUE(ota_http::isAllowedHost(renamed.host));
  EXPECT_STREQ(renamed.path, "/SemonCat/OrbitInk/releases/download/x3-ota-b1/firmware.bin");
  ASSERT_TRUE(ota_http::resolveRedirect(renamed, "https://release-assets.githubusercontent.com/fixture?token=test",
                                      asset));
  EXPECT_TRUE(ota_http::isAllowedHost(asset.host));
  // This is parser/allowlist evidence, not proof of a future live GitHub rename.
}

// Recorded GitHub release-download 302 (HTTP/1.1, 2026-10-31) with the signed token, cookies and
// request id replaced by same-length filler: the 3,994-byte Content-Security-Policy line and the
// 913-byte Location line are what the device actually parses on every OTA hop.
TEST(OtaHttp, RecordedGithubReleaseRedirectHeadParsesWithinDeviceLimits) {
  std::ifstream f(std::string(OTA_TEST_DATA_DIR) + "/github_302_head.txt", std::ios::binary);
  const std::string head{std::istreambuf_iterator<char>(f), {}};
  ASSERT_GT(head.size(), 5000u);
  for (const size_t slice : {size_t{512}, size_t{1}, size_t{4096}}) {  // device reads 512-byte slices
    ota_http::ResponseHeadParser parser;
    size_t off = 0;
    while (off < head.size() && parser.status() == ota_http::ResponseHeadParser::Status::NEED_MORE) {
      const size_t n = std::min(slice, head.size() - off);
      off += parser.feed(reinterpret_cast<const uint8_t*>(head.data()) + off, n);
    }
    ASSERT_EQ(parser.status(), ota_http::ResponseHeadParser::Status::DONE) << slice;
    EXPECT_EQ(parser.statusCode(), 302);
    ota_http::Url current{};
    ASSERT_TRUE(ota_http::parseHttpsUrl("https://github.com/SemonCat/OrbitInk/releases/download/b/f.bin", current));
    ota_http::Url next{};
    ASSERT_TRUE(ota_http::resolveRedirect(current, parser.location(), next));
    EXPECT_STREQ(next.host, "release-assets.githubusercontent.com");
    EXPECT_TRUE(ota_http::isAllowedHost(next.host));
  }
}

// ---- Firmware download resume (Range continuation) ------------------------------------------------
namespace {
ota_http::ResponseHeadParser parseHead(const std::string& head) {
  ota_http::ResponseHeadParser p;
  p.feed(reinterpret_cast<const uint8_t*>(head.data()), head.size());
  return p;
}
}  // namespace

TEST(OtaHttp, ParsesContentRangeStrictly) {
  // Shape GitHub's release host returned to "Range: bytes=2374342-" for build 1801 (2026-11-01).
  auto p = parseHead("HTTP/1.1 206 Partial Content\r\nContent-Length: 3550490\r\n"
                     "Content-Range: bytes 2374342-5924831/5924832\r\n\r\n");
  ASSERT_EQ(p.status(), ota_http::ResponseHeadParser::Status::DONE);
  ASSERT_TRUE(p.hasContentRange());
  EXPECT_EQ(p.rangeFirst(), 2374342u);
  EXPECT_EQ(p.rangeLast(), 5924831u);
  EXPECT_EQ(p.rangeTotal(), 5924832u);
  for (const char* bad : {"bytes */5924832", "bytes 1-2/*", "items 0-1/2", "bytes 5-4/9", "bytes 0-9/9", "bytes 1-2",
                          "bytes  1-2/3", "bytes 1-2/3x", "bytes -1-2/3", "bytes 1234567890123456-1/2"}) {
    auto q = parseHead(std::string("HTTP/1.1 206 Partial Content\r\nContent-Range: ") + bad + "\r\n\r\n");
    EXPECT_EQ(q.status(), ota_http::ResponseHeadParser::Status::ERROR) << bad;
  }
  auto twice = parseHead("HTTP/1.1 206 X\r\nContent-Range: bytes 0-1/2\r\nContent-Range: bytes 0-1/2\r\n\r\n");
  EXPECT_EQ(twice.status(), ota_http::ResponseHeadParser::Status::ERROR);
}

TEST(OtaHttp, ContinuationAcceptsOnlyTheExactRemainderOfTheSameObject) {
  using ota_http::Continuation;
  constexpr uint64_t total = 5924832, offset = 2374342;
  const auto check = [&](const std::string& head, uint64_t at) {
    return ota_http::checkContinuation(parseHead(head), at, total);
  };
  EXPECT_EQ(check("HTTP/1.1 200 OK\r\nContent-Length: 5924832\r\n\r\n", 0), Continuation::OK);
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 3550490\r\nContent-Range: bytes 2374342-5924831/5924832\r\n\r\n",
                  offset),
            Continuation::OK);
  // Server ignored Range and restarted at byte 0: feeding it would corrupt the stream.
  EXPECT_EQ(check("HTTP/1.1 200 OK\r\nContent-Length: 5924832\r\n\r\n", offset), Continuation::BAD_STATUS);
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 3550490\r\n\r\n", offset), Continuation::BAD_RANGE);
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 3550491\r\nContent-Range: bytes 2374341-5924831/5924832\r\n\r\n",
                  offset),
            Continuation::BAD_RANGE);  // starts one byte early
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 3550490\r\nContent-Range: bytes 2374342-5924831/5924833\r\n\r\n",
                  offset),
            Continuation::BAD_RANGE);  // a different object
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 1000\r\nContent-Range: bytes 2374342-2375341/5924832\r\n\r\n",
                  offset),
            Continuation::BAD_RANGE);  // a partial slice, not the remainder
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 999\r\nContent-Range: bytes 2374342-5924831/5924832\r\n\r\n",
                  offset),
            Continuation::BAD_LENGTH);
  EXPECT_EQ(check("HTTP/1.1 206 P\r\nContent-Length: 5924832\r\nContent-Range: bytes 0-5924831/5924832\r\n\r\n", 0),
            Continuation::BAD_STATUS);  // a fresh request must be a plain 200
  EXPECT_EQ(check("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n\r\n", offset), Continuation::BAD_STATUS);
}

TEST(OtaHttp, ResumeBudgetAllowsOneNoProgressFailureAndIsBounded) {
  constexpr uint64_t total = 5924832;
  EXPECT_TRUE(ota_http::shouldResume(0, 0, 2374342, total));
  EXPECT_TRUE(ota_http::shouldResume(1, 1, 2374342, total));  // one failed continuation may recover
  EXPECT_FALSE(ota_http::shouldResume(2, 2, 2374342, total));
  EXPECT_TRUE(ota_http::shouldResume(0, 1, 0, total));        // initial header can be interrupted
  EXPECT_FALSE(ota_http::shouldResume(0, 0, total, total));   // nothing left
  EXPECT_TRUE(ota_http::shouldResume(ota_http::MAX_RESUMES - 1, 0, 20, total));
  EXPECT_FALSE(ota_http::shouldResume(ota_http::MAX_RESUMES, 0, 20, total));
}
