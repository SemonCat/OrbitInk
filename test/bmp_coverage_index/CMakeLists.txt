add_executable(BmpCoverageIndexTest
  BmpCoverageIndexTest.cpp
)

target_include_directories(BmpCoverageIndexTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(BmpCoverageIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BmpCoverageIndexTest)
