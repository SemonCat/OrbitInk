# Tests the Journal Home geometry (src/components/themes/journal/JournalHomeLayout.h) at Latin, CJK
# fallback and reader-font headline metrics for 0/1/2 recent books, and the Journal-owned
# UTF-8/CJK-aware title wrap (JournalText.h). Header-only: no renderer, no fonts, no SD.
add_executable(JournalLayoutTest
  JournalLayoutTest.cpp
)

target_include_directories(JournalLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(JournalLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JournalLayoutTest)
