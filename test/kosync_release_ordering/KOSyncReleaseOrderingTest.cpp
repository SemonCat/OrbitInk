// Source-order guards for the retained BookOrbit transport and removed product entry points.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Returns the position of `needle` in `haystack`, failing the current test
// (via a fatal assertion message on the returned npos) rather than silently
// comparing string::npos values against each other.
size_t mustFind(const std::string& haystack, const std::string& needle, const char* what) {
  const size_t pos = haystack.find(needle);
  EXPECT_NE(pos, std::string::npos) << "expected to find " << what << " but it is missing -- "
                                    << "either the source was refactored (update this anchor) "
                                    << "or the fix it locks in was reverted.";
  return pos;
}

}  // namespace

TEST(BookOrbitReleaseOrdering, AutomaticPushReleasesAfterEpubAndAgainBeforePut) {
  const std::string src = readFile(REPO_ROOT "/src/util/BookOrbitAutoSync.cpp");
  ASSERT_FALSE(src.empty());
  const size_t reset = mustFind(src, "epub.reset();", "EPUB release");
  const size_t connect = mustFind(src, "if (!connectToSavedWifi())", "WiFi connection");
  const size_t release = src.find("forceReleaseTransientCaches()", reset);
  EXPECT_LT(reset, release);
  EXPECT_LT(release, connect);
  const size_t put = mustFind(src, "session.updateProgressAndVerify(toPush, userRequestedAbort)", "progress PUT");
  const size_t secondRelease = src.rfind("forceReleaseTransientCaches()", put);
  EXPECT_LT(connect, secondRelease);
  EXPECT_LT(secondRelease, put);
}

TEST(BookOrbitProduct, RetiredProviderHasNoUiOrNetworkEntryPoints) {
  for (const char* path : {"/src/activities/reader/EpubReaderActivity.cpp", "/src/activities/ActivityManager.cpp",
                           "/src/main.cpp"}) {
    const std::string src = readFile(std::string(REPO_ROOT) + path);
    ASSERT_FALSE(src.empty());
    EXPECT_EQ(src.find("KOReaderAutoSync::sync"), std::string::npos) << path;
    EXPECT_EQ(src.find("launchKOReaderSync"), std::string::npos) << path;
  }
  const auto web = readFile(REPO_ROOT "/src/network/CrossPointWebServer.cpp");
  const auto page = readFile(REPO_ROOT "/src/network/html/SettingsPage.html");
  EXPECT_EQ(web.find("\"/api/kosync"), std::string::npos);
  EXPECT_EQ(page.find("/api/kosync"), std::string::npos);
  EXPECT_EQ(page.find("renderKOSyncSection"), std::string::npos);
}
