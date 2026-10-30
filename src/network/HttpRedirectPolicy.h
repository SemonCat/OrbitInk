#pragma once
#include <string>
#include <utility>

// Pure, host-testable redirect-origin policy for HttpDownloader.cpp's hand-rolled wolfSSL
// redirect loop (runGetWolf() -- see that function's own comment for why it does not use
// freeink::SecureHttpClient's built-in setFollowRedirects()/setAllowRedirectDowngrade(), which
// already apply an equivalent downgrade check but are not used by the download path).
//
// No Arduino/WiFi/ESP dependency on purpose: HttpDownloader.cpp itself pulls in Arduino.h and the
// wolfSSL/esp_http_client transports, which would drag a large stub surface into a host test for
// what is otherwise a few lines of string comparison.
namespace HttpRedirectPolicy {

// True when a redirect from `fromUrl` to `toUrl` may carry forward Authorization / caller-supplied
// extra headers (which include BookOrbit's x-auth-user/x-auth-key): same scheme, host, AND port.
// Also false whenever either URL cannot be parsed as an http(s) URL -- fails closed rather than
// forwarding credentials on an unrecognized shape.
bool shouldForwardAuthOnRedirect(const std::string& fromUrl, const std::string& toUrl);

// True when following the redirect would step down from https to http, dropping transport
// security for every subsequent hop. Independent of shouldForwardAuthOnRedirect(): a same-origin
// downgrade (rare, but not impossible for a misconfigured server) is still a downgrade.
bool isRedirectDowngrade(const std::string& fromUrl, const std::string& toUrl);

// Tracks the ORIGINAL request's origin across an entire redirect chain and decides, hop by hop,
// whether the current hop may still carry Authorization / caller-supplied extra headers.
//
// Deliberately compares every hop against the chain's START, never against the immediately
// preceding hop: comparing hop-to-hop lets the effective origin "drift" one step at a time, which
// a same-origin-to-EACH-OTHER but off-original attacker chain defeats -- e.g.
// trusted -> evil/x -> evil/y: a previous-hop comparison correctly strips credentials going from
// trusted to evil/x, then WRONGLY re-adds them going from evil/x to evil/y, because those two
// hops share an origin with each other even though neither is the original trusted one. Comparing
// against the fixed start closes that: evil/y is never same-origin with the trusted start, so
// every off-original hop stays stripped. A later hop that genuinely returns to the original
// origin may carry credentials again; they still never go anywhere except that original origin.
class RedirectAuthTracker {
 public:
  explicit RedirectAuthTracker(std::string startUrl) : startUrl_(std::move(startUrl)) {}

  // `hopUrl` is the URL about to be requested -- including the very first request (hop 0), which
  // is trivially same-origin with itself, so no separate "is this hop 0" case is needed.
  bool shouldForwardAuth(const std::string& hopUrl) const { return shouldForwardAuthOnRedirect(startUrl_, hopUrl); }

 private:
  std::string startUrl_;
};

}  // namespace HttpRedirectPolicy
