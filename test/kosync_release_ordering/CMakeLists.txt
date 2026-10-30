# Pure source-text test: no HAL/Arduino stubbing needed, just reads the real
# .cpp files from the repo and asserts relative substring positions. See the
# top-of-file comment in KOSyncReleaseOrderingTest.cpp for why this seam (and
# not a runtime test) is the appropriate one for these two ordering fixes.
add_executable(KOSyncReleaseOrderingTest
  KOSyncReleaseOrderingTest.cpp
)

target_compile_definitions(KOSyncReleaseOrderingTest PRIVATE
  REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(KOSyncReleaseOrderingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOSyncReleaseOrderingTest)
