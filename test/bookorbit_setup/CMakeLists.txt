# First-run BookOrbit setup guide decisions (src/activities/settings/BookOrbitSetupPolicy.h) and the
# network mode list shown for file transfer vs BookOrbit web setup (src/activities/network/NetworkModeList.h).
# Header-only: no renderer, no WiFi, no SD.
add_executable(BookOrbitSetupTest
  BookOrbitSetupTest.cpp
)

target_include_directories(BookOrbitSetupTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BookOrbitSetupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitSetupTest)
