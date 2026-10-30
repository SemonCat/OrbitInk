// Locks in the call-ORDER invariants from the Ruby acceptance-inspection
// follow-up to b1c2b4d ("fix: force-release SD font mini-bitmaps before
// KOSync TLS"): two in-scope gaps where a release call existed but ran too
// late (or not at all) relative to further EPUB allocation / the actual TLS
// request that follows it.
//
// A full runtime test of these call sites would need to stub GfxRenderer,
// ActivityManager, WiFi, and Epub -- disproportionate for what is fundamentally
// a source-ordering property ("call A happens before call B in this
// function"), and the kind of thing a future edit could silently reorder
// without any of the existing unit tests noticing (FontCacheManager's own
// release logic is already covered by test/sdcardfont_transient_release/;
// this test is specifically about WHERE the calls sit, not whether the
// release logic itself works). Reading the real source as text and asserting
// relative substring positions is the smallest seam that actually catches a
// regression here: if either ordering is undone, this test fails immediately
// without needing to build or run any device code.

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

TEST(KOSyncReleaseOrdering, ManualSyncReleasesBeforeProgressMapping) {
  // Gap 1 (Ruby): EpubReaderActivity::launchKOReaderSync() called
  // ProgressMapper::toSavedProgress() -- which can do further EPUB
  // parsing/inflate work -- before any force-release ran, since
  // KOReaderSyncActivity's own release calls don't exist yet at that point
  // (the sync activity hasn't even been constructed). The user's real device
  // log showed ZIP inflate failures before NTP/TLS ever started, consistent
  // with allocation pressure at exactly this point.
  const std::string src = readFile(REPO_ROOT "/src/activities/reader/EpubReaderActivity.cpp");
  ASSERT_FALSE(src.empty()) << "could not read EpubReaderActivity.cpp -- check REPO_ROOT";

  const size_t fnPos = mustFind(src, "bool EpubReaderActivity::launchKOReaderSync() {", "launchKOReaderSync()");
  const size_t releasePos =
      mustFind(src, "if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();",
               "the force-release call");
  const size_t mappingPos = mustFind(
      src, "SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);", "toSavedProgress()");

  EXPECT_LT(fnPos, releasePos) << "the force-release call must be inside launchKOReaderSync()";
  EXPECT_LT(releasePos, mappingPos) << "the force-release call must run BEFORE toSavedProgress(), "
                                       "not after -- that ordering is the whole point of this fix";
}

TEST(KOSyncReleaseOrdering, AutomaticPushReleasesAgainRightBeforeThePut) {
  // Gap 2 (Ruby): KOReaderAutoSync::pushCurrentProgress() released before the
  // EPUB reload, then kept the shared_ptr<Epub> alive through
  // ProgressMapper::toSavedProgress() and straight into the PUT (originally
  // the static KOReaderSyncClient::updateProgress(), now routed through the
  // reused-transport Session::updateProgress()) -- so the earlier release did
  // not actually cover the second TLS request, despite a comment implying it
  // did. The fix: free the Epub and release again immediately before the
  // PUT, once toPush no longer needs it.
  const std::string src = readFile(REPO_ROOT "/src/util/KOReaderAutoSync.cpp");
  ASSERT_FALSE(src.empty()) << "could not read KOReaderAutoSync.cpp -- check REPO_ROOT";

  // The unified transaction accepts a null renderer for display-less wakes, so the release is
  // guarded. Match that adjacent block and separately require the second guarded release before
  // PUT; together they preserve the old heap invariant without dereferencing headless state.
  const size_t resetThenReleasePos =
      mustFind(src,
               "epub.reset();\n"
               "  if (renderer) {\n"
               "    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();\n"
               "  }",
               "epub.reset() immediately followed by the force-release call");

  const size_t putPos =
      mustFind(src, "session.updateProgress(toPush, userRequestedAbort);", "the updateProgress() PUT call");
  const size_t secondReleasePos = src.rfind("renderer->getFontCacheManager()", putPos);

  EXPECT_LT(resetThenReleasePos, putPos)
      << "epub.reset() + forceReleaseTransientCaches() must run before the PUT";
  ASSERT_NE(secondReleasePos, std::string::npos);
  EXPECT_LT(resetThenReleasePos, secondReleasePos);
  EXPECT_LT(secondReleasePos, putPos) << "transient caches must be released again immediately before the PUT";

  // Also confirm the comment no longer overclaims coverage of the PUT -- the
  // exact wording from before this fix, which this test would otherwise not
  // notice was left behind (it doesn't affect behavior, but leaving a false
  // claim in a safety-relevant comment is exactly the kind of thing that
  // misleads the next person touching this function).
  EXPECT_EQ(src.find("and the second TLS request\n  // (updateProgress) that follows it"), std::string::npos)
      << "a stale comment claiming the earlier release covers the PUT is still present";
}
