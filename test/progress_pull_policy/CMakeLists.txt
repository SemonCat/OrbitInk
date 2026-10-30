# Pure, header-only decision logic shared by KOReaderAutoSync.cpp and BookOrbitAutoSync.cpp (both
# untestable at the host level -- they depend directly on Arduino/WiFi/Epub) -- see
# lib/KOReaderSync/ProgressPullPolicy.h. No stubs needed: two float comparisons and nothing else.
add_executable(ProgressPullPolicyTest
  ProgressPullPolicyTest.cpp
)
target_include_directories(ProgressPullPolicyTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)
target_link_libraries(ProgressPullPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(ProgressPullPolicyTest)
