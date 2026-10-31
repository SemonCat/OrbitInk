add_executable(CrashReportTest CrashReportTest.cpp)
target_include_directories(CrashReportTest PRIVATE ${REPO_ROOT}/lib/hal)
target_link_libraries(CrashReportTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CrashReportTest)
