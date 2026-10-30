# Tests the pure decision logic in src/activities/reader/RefreshEligibility.h -- no I/O, no
# display/HAL stack needed.
add_executable(RefreshEligibilityTest
  RefreshEligibilityTest.cpp
)

target_include_directories(RefreshEligibilityTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(RefreshEligibilityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RefreshEligibilityTest)
