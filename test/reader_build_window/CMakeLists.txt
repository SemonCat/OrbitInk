# Tests the pure decision logic in src/activities/reader/BuildWindow.h -- no I/O, no
# Arduino/Section/HalStorage stack needed.
add_executable(ReaderBuildWindowTest
  ReaderBuildWindowTest.cpp
)

target_include_directories(ReaderBuildWindowTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderBuildWindowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderBuildWindowTest)
