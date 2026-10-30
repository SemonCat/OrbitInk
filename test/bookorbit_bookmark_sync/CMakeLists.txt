# BookOrbit bookmark-sync PURE decision logic (src/util/BookOrbitBookmarkSyncPolicy.cpp) --
# minting identities, building the outgoing batch, and applying incoming changes. Deliberately
# lean: these functions take plain data in and produce plain data out with no I/O, so this target
# needs no HalStorage/HalClock/CrossPointSettings stub at all -- only the identity-key digest
# (BookOrbitBookmarks.cpp, which needs the shared MD5Builder.h stub) and BookmarkUtil.cpp (pure,
# used for incoming-title sanitization).
#
# The orchestrating BookOrbitBookmarkSync::sync() (src/util/BookOrbitBookmarkSync.cpp) is NOT
# compiled or tested here: it depends directly on HalClock/HalStorage/CrossPointSettings/an open
# BookOrbitSyncClient::Session, matching KOReaderAutoSync.cpp/BookOrbitAutoSync.cpp's own
# established precedent of leaving that orchestration layer host-untested -- see
# docs/bookorbit-feature-matrix.md's Phase 4 test-coverage section.
add_executable(BookOrbitBookmarkSyncPolicyTest
  BookOrbitBookmarkSyncPolicyTest.cpp
  ${REPO_ROOT}/src/util/BookOrbitBookmarkSyncPolicy.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitBookmarks.cpp
)
target_include_directories(BookOrbitBookmarkSyncPolicyTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
  # MD5Builder.h stand-in, shared with test/kosync_docid rather than duplicated.
  ${REPO_ROOT}/test/kosync_docid/host_stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
)
target_link_libraries(BookOrbitBookmarkSyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BookOrbitBookmarkSyncPolicyTest)
