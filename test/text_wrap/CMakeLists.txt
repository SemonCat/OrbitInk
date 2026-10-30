# Tests the pure line-break core in lib/GfxRenderer/TextWrap.h against a reference
# implementation of the quadratic search it replaces. Only Utf8.cpp is needed -- no
# renderer, no fonts, no SD.
add_executable(TextWrapTest
  TextWrapTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TextWrapTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TextWrapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextWrapTest)
