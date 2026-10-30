# Tests the CSS rules cache record walk (lib/Epub/Epub/css/CssCacheLayout.h) against byte images
# -- no filesystem, no HalStorage. This is the arithmetic that decides whether a warm book open
# reuses the CSS cache or throws it away and reparses.
add_executable(CssCacheLayoutTest
  CssCacheLayoutTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssCacheLayout.cpp
)

target_include_directories(CssCacheLayoutTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub/css
)

target_link_libraries(CssCacheLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssCacheLayoutTest)
