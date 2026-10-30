# Tests the one-shot auto-sleep re-base gate in
# src/activities/settings/IdleBaselineLatch.h -- pure state, no activity stack, no network.
add_executable(IdleBaselineLatchTest
  IdleBaselineLatchTest.cpp
)

target_include_directories(IdleBaselineLatchTest PRIVATE
  ${REPO_ROOT}/src/activities/settings
)

target_link_libraries(IdleBaselineLatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IdleBaselineLatchTest)
