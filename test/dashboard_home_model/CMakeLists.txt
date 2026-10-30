# Tests the pure book-selection logic in src/activities/home/DashboardHomeModel.h -- no I/O, no
# renderer, no HAL, so the Dashboard's whole selection contract (pinned default, ephemeral browse,
# degradation to the empty state) is checkable on the host.
add_executable(DashboardHomeModelTest
  DashboardHomeModelTest.cpp
)

target_include_directories(DashboardHomeModelTest PRIVATE
  ${REPO_ROOT}/src/activities/home
)

target_link_libraries(DashboardHomeModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardHomeModelTest)
