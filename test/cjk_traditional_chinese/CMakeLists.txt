add_executable(CjkTraditionalChineseTest
  CjkTraditionalChineseTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(CjkTraditionalChineseTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(CjkTraditionalChineseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CjkTraditionalChineseTest)
