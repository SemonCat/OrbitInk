# Tests for the REAL src/util/BookOrbitMigration.cpp against the real
# BookOrbitStatsQueue/BookOrbitOwnerId sources, an in-memory read-write HalStorage stub, and
# test-settable credential stores. See BookOrbitMigrationTest.cpp's file comment for what this
# covers versus test/bookorbit_stats/BookOrbitQueueTest.cpp's direct rewriteOwner() coverage.
add_executable(BookOrbitMigrationTest
  BookOrbitMigrationTest.cpp
  ${REPO_ROOT}/src/util/BookOrbitMigration.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitStatsQueue.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
)

target_include_directories(BookOrbitMigrationTest PRIVATE
  # This directory's own CrossPointState stub carries the migration-relevant fields
  # (koStatsServerId/Kind, boMigrationAttempted); the writable HalStorage stub is shared with
  # test/bookorbit_stats rather than duplicated.
  ${REPO_ROOT}/test/bookorbit_sync/host_stubs
  ${REPO_ROOT}/test/bookorbit_stats/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/src
)

target_link_libraries(BookOrbitMigrationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BookOrbitMigrationTest)
