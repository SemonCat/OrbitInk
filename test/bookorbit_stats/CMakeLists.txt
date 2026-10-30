# BookOrbit page-stats: host test targets over the REAL lib/KOReaderSync sources, split by what
# each one needs to stand in for.
#
#  - BookOrbitSessionClockTest: header-only clock, no stubs at all beyond lib/hal's pure
#    CalendarTime.
#  - BookOrbitQueueTest: needs a READ-WRITE HalStorage (durability is the whole point), so it
#    uses this directory's own writable in-memory stub rather than the read-only ones in
#    test/image_zip_hardening/ and test/sdcardfont_transient_release/.
#
# The BookOrbit CLIENT protocol suite (detectBookOrbit/uploadPageStats, now
# BookOrbitSyncClient::Session::probeCapabilities/uploadPageStats) lives in
# test/bookorbit_sync/BookOrbitClientProtocolTest.cpp instead of here -- it moved off
# KOReaderSyncClient::Session onto a standalone BookOrbitSyncClient when BookOrbit became an
# explicit provider (see docs/bookorbit-feature-matrix.md), so it now belongs next to the policy
# suite that exercises the same client.

# ---- Session clock (pure) --------------------------------------------------------------------
add_executable(BookOrbitSessionClockTest
  BookOrbitSessionClockTest.cpp
)
target_include_directories(BookOrbitSessionClockTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(BookOrbitSessionClockTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BookOrbitSessionClockTest)

# ---- Durable queue ---------------------------------------------------------------------------
add_executable(BookOrbitQueueTest
  BookOrbitQueueTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitStatsQueue.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
)
target_include_directories(BookOrbitQueueTest PRIVATE
  # This directory's writable HalStorage stub comes first; test/kosync_protocol/host_stubs then
  # supplies Logging.h (device UART logging) unchanged, so the two suites share one definition.
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
)
target_link_libraries(BookOrbitQueueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BookOrbitQueueTest)

# ---- Owner fingerprint and per-installation BookOrbit id ---------------------------------------
# Compiles the production identity sources. The SHA-256 itself is the firmware's mbedTLS (see
# BookOrbitOwnerIdSha256.cpp, firmware-only), so these targets inject a deterministic digest and
# verify the parts this feature owns: the length-framed owner encoding, plus the random
# installation-id format and entropy spread BookOrbit's eight-character session prefix depends on.
add_executable(BookOrbitIdentityTest
  BookOrbitIdentityTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitStatsDeviceId.cpp
)
target_include_directories(BookOrbitIdentityTest PRIVATE ${REPO_ROOT}/lib/KOReaderSync)
target_link_libraries(BookOrbitIdentityTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitIdentityTest)

# ---- Reading-stats session finalization --------------------------------------------------------
# Header-only production logic; BookReadingStats/GlobalReadingStats are included for their plain
# data members only, so no SD or Arduino stack is needed.
add_executable(ReadingStatsSessionTest
  ReadingStatsSessionTest.cpp
)
target_include_directories(ReadingStatsSessionTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/src/activities/reader
)
target_link_libraries(ReadingStatsSessionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingStatsSessionTest)

# ---- Page dwell measurement --------------------------------------------------------------------
# Header-only production logic with an injected clock, so the "every page-leave closes the
# outgoing dwell" rule that non-page-turn navigation kept breaking is testable exactly, without
# the Activity/renderer/SD stack.
add_executable(PageDwellTrackerTest
  PageDwellTrackerTest.cpp
)
target_include_directories(PageDwellTrackerTest PRIVATE ${REPO_ROOT}/src/activities/reader)
target_link_libraries(PageDwellTrackerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PageDwellTrackerTest)
