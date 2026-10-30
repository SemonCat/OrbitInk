# Tests the pure decision logic in src/activities/reader/ProgressDebounce.h -- no I/O, no
# Arduino/HalStorage stack needed.
add_executable(ProgressDebounceTest
  ProgressDebounceTest.cpp
)

target_include_directories(ProgressDebounceTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressDebounceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressDebounceTest)
