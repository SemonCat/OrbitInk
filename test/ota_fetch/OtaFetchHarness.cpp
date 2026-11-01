// Executable harness for the production fetch/redirect/read/resume loops.
#include <OtaFetch.h>
#include <SecureClient.h>
#include <OtaHttp.h>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace fixture {
std::vector<Reply> replies;
std::vector<std::string> requests, hosts;
size_t next = 0;
unsigned long clock = 0;
int watchdog = 0, live = 0, peak = 0;
}
const char* URL = "https://github.com/fixture/releases/download/test/firmware.bin";
const char* ASSET = "https://release-assets.githubusercontent.com/fixture?signature=not-a-secret";
std::string body;
bool sink(const uint8_t* p, size_t n, void*) { body.append(reinterpret_cast<const char*>(p), n); return true; }
bool reject(const uint8_t*, size_t, void*) { return false; }
std::string response(size_t n, size_t offset = 0, size_t total = 0) {
  return "HTTP/1.1 " + std::string(offset ? "206 Partial Content" : "200 OK") + "\r\nContent-Length: " +
         std::to_string(n) + (offset ? "\r\nContent-Range: bytes " + std::to_string(offset) + "-" +
         std::to_string(total - 1) + "/" + std::to_string(total) : "") + "\r\n\r\n";
}
void direct(std::string head, std::string data, int end = -1, int waits = 0) {
  fixture::replies.push_back({head, data, end, waits});
}
void redirected(size_t offset, size_t total, std::string data, int end = -1) {
  direct("HTTP/1.1 302 Found\r\nLocation: https://objects.githubusercontent.com/fresh\r\n\r\n", "");
  direct(std::string("HTTP/1.1 302 Found\r\nLocation: ") + ASSET + "\r\n\r\n", "");
  direct(response(total - offset, offset, total), data, end);
}
void reset() {
  assert(fixture::live == 0);
  fixture::replies.clear(); fixture::requests.clear(); fixture::hosts.clear();fixture::next = 0;
  fixture::clock = 0; fixture::watchdog = 0; fixture::peak = 0; body.clear();
}
ota_fetch::Result fetch(size_t size, ota_diagnostics::Record& d, int& status, bool manifest = false) {
  return manifest ? ota_fetch::get(URL, "fixture", size, size, sink, nullptr, &status, &d) :
                    ota_fetch::getResumable(URL, "fixture", size, sink, nullptr, &status, &d);
}
int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  reset(); ota_diagnostics::Record d; int status = 0;
  const std::string first(49152, 'A'), rest(1024, 'B');
  if (scenario == "report" || scenario == "recover-no-progress") {
    const size_t total = scenario == "report" ? 5907424 : first.size() + rest.size();
    redirected(0, total, first); redirected(first.size(), total, "");
    redirected(first.size(), total, scenario == "report" ? "" : rest);
    auto result = fetch(total, d, status);
    std::cout << ota_fetch::resultName(result) << "/" << d.detail << " HTTP" << status << " hop" << d.hop
              << " resumes" << unsigned(d.resumes) << " bytes" << body.size() << "/" << total << std::endl;
    if (scenario == "report") {
      assert(result == ota_fetch::Result::TRUNCATED && status == 206 && d.resumes == 2 && d.hop == 2);
      assert(body.size() == 49152 && std::string(d.detail) == "TLS_READ_ERROR");
      assert(d.noProgressAttempts == 2 && std::string(d.retryStop) == "NO_PROGRESS_LIMIT");
    } else {
      assert(result == ota_fetch::Result::OK); assert(body == first + rest);
      assert(d.resumes == 2 && fixture::next == 9);
    }
  } else if (scenario == "transient-head-body") {
    // SecureClient maps wolfSSL WANT_READ/WANT_WRITE to 0, with the session alive.
    direct(response(8), "abcdefgh", -1, 10);
    assert(fetch(8, d, status, true) == ota_fetch::Result::OK);
    assert(body == "abcdefgh" && fixture::next == 1 && fixture::watchdog >= 20);
  } else if (scenario == "coalesced") {
    direct(response(8) + "abcdefgh", "");
    assert(fetch(8,d,status)==ota_fetch::Result::OK && body=="abcdefgh" && fixture::next==1);
  } else if (scenario == "bounded-idle") {
    for (int i = 0; i < 5; ++i) direct(response(8), "", 0);
    assert(fetch(8, d, status) == ota_fetch::Result::TIMEOUT);
    assert(fixture::next == 2 && fixture::clock < 43000 && body.empty());
    assert(d.noProgressAttempts == 2 && std::string(d.retryStop) == "NO_PROGRESS_LIMIT");
  } else if (scenario == "disconnect-resume") {
    direct(response(8), "abcd", -2); direct(response(4, 4, 8), "efgh");
    assert(fetch(8, d, status) == ota_fetch::Result::OK && body == "abcdefgh");
    assert(fixture::requests[1].find("Range: bytes=4-") != std::string::npos);
  } else if (scenario == "header-retry") {
    direct("HTTP/1.1 200", "", -1);direct(response(8), "abcdefgh");
    assert(fetch(8, d, status) == ota_fetch::Result::OK && body == "abcdefgh");
    assert(fixture::requests[1].find("Range:") == std::string::npos);
  } else if (scenario == "resume-header-retry") {
    direct(response(8), "abcd"); direct("HTTP/1.1 206", "");direct(response(4, 4, 8), "efgh");
    assert(fetch(8, d, status) == ota_fetch::Result::OK && body == "abcdefgh");
    assert(fixture::requests[2].find("Range: bytes=4-") != std::string::npos);
  } else if (scenario == "wrong-range") {
    direct(response(8), "abcd");direct(response(5, 3, 8), "WRONG");
    assert(fetch(8, d, status) == ota_fetch::Result::BAD_RESPONSE && body == "abcd");
    assert(fixture::next == 2 && std::string(d.detail) == "RESUME_BAD_RANGE");
  } else if (scenario == "budget") {
    for (size_t i = 0; i < 5; ++i) direct(response(8-i, i, 8), "a");
    assert(fetch(8, d, status) == ota_fetch::Result::TRUNCATED && body == "aaaa");
    assert(fixture::next == 4 && d.resumes == 3);
    assert(std::string(d.retryStop) == "RESUME_LIMIT");
  } else if (scenario == "sink") {
    direct(response(8), "abcdefgh");
    assert(ota_fetch::getResumable(URL,"fixture",8,reject,nullptr,&status,&d)==ota_fetch::Result::SINK_ABORTED);
    assert(fixture::next == 1);
  } else if (scenario == "manifest-single") {
    direct(response(8), "abcd");direct(response(4,4,8), "efgh");
    assert(fetch(8,d,status,true)==ota_fetch::Result::TRUNCATED && fixture::next==1);
  } else if (scenario == "host-policy") {
    direct("HTTP/1.1 302 Found\r\nLocation: https://attacker.invalid/file\r\n\r\n", "");
    assert(fetch(8,d,status)==ota_fetch::Result::HOST_NOT_ALLOWED && fixture::next==1);
  } else if (scenario == "header-idle") {
    direct("HTTP/1.1 200", "", 0); direct("HTTP/1.1 200", "", 0);
    assert(fetch(8,d,status)==ota_fetch::Result::TIMEOUT && fixture::next==2);
    assert(std::string(d.detail)=="HEADER_IDLE" && fixture::clock<43000);
  } else if (scenario == "repeated-disconnect") {
    direct(response(8), "", -2); direct(response(8), "", -2);
    assert(fetch(8,d,status)==ota_fetch::Result::TRUNCATED && fixture::next==2);
    assert(std::string(d.detail)=="PEER_CLOSED" && d.clientReadResult==0);
    assert(d.noProgressAttempts==2 && body.empty());
  } else if (scenario == "progress-reset") {
    direct(response(8), ""); direct(response(8), "abcd");
    direct(response(4,4,8), ""); direct(response(4,4,8), "efgh");
    assert(fetch(8,d,status)==ota_fetch::Result::OK && body=="abcdefgh" && d.resumes==3);
  } else if (scenario == "connect-retry" || scenario == "write-retry") {
    direct("", "");
    if (scenario=="connect-retry") fixture::replies.back().connectOk=false;
    else fixture::replies.back().writeOk=false;
    direct(response(8), "abcdefgh");
    assert(fetch(8,d,status)==ota_fetch::Result::OK && body=="abcdefgh" && fixture::next==2);
  } else if (scenario == "malformed" || scenario == "http-error" || scenario == "chunked") {
    if (scenario=="malformed") direct("not HTTP\r\n\r\n", "");
    else if (scenario=="http-error") direct("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", "");
    else direct("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", "8\r\nabcdefgh\r\n0\r\n\r\n");
    const auto result=fetch(8,d,status);
    assert(result==(scenario=="malformed" ? ota_fetch::Result::BAD_RESPONSE :
                   scenario=="http-error" ? ota_fetch::Result::HTTP_STATUS : ota_fetch::Result::NO_LENGTH));
    assert(fixture::next==1 && body.empty() && std::string(d.retryStop)=="NOT_RETRYABLE");
  } else if (scenario == "ignored-range" || scenario == "wrong-length") {
    direct(response(8), "abcd");
    direct(scenario=="ignored-range" ? response(8) : response(3,4,8), "BAD");
    assert(fetch(8,d,status)==ota_fetch::Result::BAD_RESPONSE && body=="abcd" && fixture::next==2);
    assert(std::string(d.detail)==(scenario=="ignored-range" ? "RESUME_BAD_STATUS" : "RESUME_BAD_LENGTH"));
  } else { assert(false); }
  assert(fixture::live == 0 && fixture::peak == 1);
}
