add_executable(KOSyncStatusTest
  KOSyncStatusTest.cpp
)

target_include_directories(KOSyncStatusTest PRIVATE
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(KOSyncStatusTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOSyncStatusTest)
