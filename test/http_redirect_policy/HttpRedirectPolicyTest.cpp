// Tests for the pure redirect-origin policy HttpDownloader.cpp's wolfSSL redirect loop
// (runGetWolf()) uses to decide whether to reforward Authorization / caller-supplied extra
// headers (which include BookOrbit's x-auth-user/x-auth-key) to a redirect target, and whether to
// refuse an https -> http downgrade. See HttpRedirectPolicy.h's own file comment for why this is
// split out of HttpDownloader.cpp: that file pulls in Arduino.h and the wolfSSL/esp_http_client
// transports, which would drag a large stub surface into a host test for a few lines of string
// comparison.

#include <gtest/gtest.h>

#include "HttpRedirectPolicy.h"

namespace {

using HttpRedirectPolicy::isRedirectDowngrade;
using HttpRedirectPolicy::RedirectAuthTracker;
using HttpRedirectPolicy::shouldForwardAuthOnRedirect;

TEST(HttpRedirectPolicyTest, SameOriginRedirectForwardsAuth) {
  EXPECT_TRUE(shouldForwardAuthOnRedirect("https://books.example.test/a", "https://books.example.test/b"));
}

TEST(HttpRedirectPolicyTest, SameOriginWithExplicitDefaultPortForwardsAuth) {
  // 443 is https's default port whether or not the URL spells it out -- the two must compare
  // equal, not merely byte-equal.
  EXPECT_TRUE(shouldForwardAuthOnRedirect("https://books.example.test/a", "https://books.example.test:443/b"));
}

TEST(HttpRedirectPolicyTest, DifferentHostStripsAuth) {
  EXPECT_FALSE(shouldForwardAuthOnRedirect("https://books.example.test/a", "https://evil.example.test/a"));
}

TEST(HttpRedirectPolicyTest, DifferentPortStripsAuth) {
  EXPECT_FALSE(shouldForwardAuthOnRedirect("https://books.example.test/a", "https://books.example.test:8443/a"));
}

TEST(HttpRedirectPolicyTest, DifferentSchemeStripsAuth) {
  EXPECT_FALSE(shouldForwardAuthOnRedirect("https://books.example.test/a", "http://books.example.test/a"));
}

TEST(HttpRedirectPolicyTest, SchemeComparisonIsCaseInsensitive) {
  EXPECT_TRUE(shouldForwardAuthOnRedirect("HTTPS://books.example.test/a", "https://books.example.test/b"));
}

TEST(HttpRedirectPolicyTest, HostComparisonIsCaseInsensitive) {
  EXPECT_TRUE(shouldForwardAuthOnRedirect("https://Books.Example.Test/a", "https://books.example.test/b"));
}

TEST(HttpRedirectPolicyTest, UnparseableUrlsFailClosed) {
  EXPECT_FALSE(shouldForwardAuthOnRedirect("not-a-url", "https://books.example.test/b"));
  EXPECT_FALSE(shouldForwardAuthOnRedirect("https://books.example.test/a", "not-a-url"));
  EXPECT_FALSE(shouldForwardAuthOnRedirect("ftp://books.example.test/a", "ftp://books.example.test/b"));
}

TEST(HttpRedirectPolicyTest, HttpsToHttpIsADowngrade) {
  EXPECT_TRUE(isRedirectDowngrade("https://books.example.test/a", "http://books.example.test/a"));
}

TEST(HttpRedirectPolicyTest, HttpToHttpsIsNotADowngrade) {
  EXPECT_FALSE(isRedirectDowngrade("http://books.example.test/a", "https://books.example.test/a"));
}

TEST(HttpRedirectPolicyTest, SameSchemeIsNotADowngrade) {
  EXPECT_FALSE(isRedirectDowngrade("https://books.example.test/a", "https://other.example.test/a"));
}

TEST(HttpRedirectPolicyTest, ADowngradeIsRefusedEvenWhenTheHostStaysTheSame) {
  // A same-origin downgrade is rarer (a misconfigured server) but still a real transport-security
  // drop -- independent of the auth-forwarding decision above.
  EXPECT_TRUE(isRedirectDowngrade("https://books.example.test/a", "http://books.example.test/a"));
  EXPECT_FALSE(shouldForwardAuthOnRedirect("https://books.example.test/a", "http://books.example.test/a"))
      << "the scheme differs, so auth is stripped here regardless of the downgrade check";
}

// ---- RedirectAuthTracker: multi-hop chain state, not just the single-pair predicate -----------

TEST(RedirectAuthTrackerTest, FirstRequestAlwaysForwardsAuth) {
  const RedirectAuthTracker tracker("https://trusted.example.test/file");
  EXPECT_TRUE(tracker.shouldForwardAuth("https://trusted.example.test/file"));
}

TEST(RedirectAuthTrackerTest, SameOriginRedirectStillForwards) {
  const RedirectAuthTracker tracker("https://trusted.example.test/file");
  EXPECT_TRUE(tracker.shouldForwardAuth("https://trusted.example.test/other-path"));
}

TEST(RedirectAuthTrackerTest, ACrossOriginHopStripsAuth) {
  const RedirectAuthTracker tracker("https://trusted.example.test/file");
  EXPECT_FALSE(tracker.shouldForwardAuth("https://evil.example.test/x"));
}

TEST(RedirectAuthTrackerTest, TwoHopChainToASameOriginPairDoesNotRegainAuth) {
  // Regression (found by review): comparing each hop against only the IMMEDIATELY PRECEDING one
  // lets the effective origin drift -- trusted -> evil/x correctly strips credentials, but a
  // hop-to-hop comparison would then see evil/x -> evil/y as "same origin" and wrongly re-add
  // them. Comparing every hop against the fixed chain START closes this: evil/y is never
  // same-origin with the trusted start, so every off-original hop stays stripped.
  const RedirectAuthTracker tracker("https://trusted.example.test/file");
  EXPECT_TRUE(tracker.shouldForwardAuth("https://trusted.example.test/file"));  // hop 0
  EXPECT_FALSE(tracker.shouldForwardAuth("https://evil.example.test/x"));       // hop 1: cross-origin, stripped
  EXPECT_FALSE(tracker.shouldForwardAuth("https://evil.example.test/y"))        // hop 2: still evil, still stripped
      << "must not regain auth just because this hop shares an origin with the PREVIOUS hop";
}

TEST(RedirectAuthTrackerTest, ReturningToTheOriginalOriginAfterALeavingHopForwardsAgain) {
  // An off-origin server may choose the next Location, but credentials still go only to the
  // original trusted origin. This confirms the comparison is against that fixed ORIGIN, not a
  // one-shot "ever left" latch.
  const RedirectAuthTracker tracker("https://trusted.example.test/a");
  EXPECT_FALSE(tracker.shouldForwardAuth("https://other.example.test/b"));
  EXPECT_TRUE(tracker.shouldForwardAuth("https://trusted.example.test/c"));
}

}  // namespace
