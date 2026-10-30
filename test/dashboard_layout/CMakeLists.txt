# Tests the pure card geometry in src/components/themes/dashboard/DashboardLayout.h. Every
# invariant is asserted at both the built-in Latin line heights and the taller shared 14pt CJK
# fallback the X3 zh-TW profile renders -- a card that only holds together at Latin metrics is a
# real, ship-able bug that no simulator screenshot from this repo would show. Header-only, so only
# BaseTheme.h's Rect is needed: no renderer, no fonts, no SD.
add_executable(DashboardLayoutTest
  DashboardLayoutTest.cpp
)

target_include_directories(DashboardLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DashboardLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardLayoutTest)
