# Tests the pure decision logic in src/activities/reader/QuickResumeEligibility.h -- no I/O, no
# Epub/Section/display stack needed.
add_executable(QuickResumeEligibilityTest
  QuickResumeEligibilityTest.cpp
)

target_include_directories(QuickResumeEligibilityTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(QuickResumeEligibilityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QuickResumeEligibilityTest)
