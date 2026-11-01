add_executable(SleepScreenPolicyTest SleepScreenPolicyTest.cpp)
target_include_directories(SleepScreenPolicyTest PRIVATE
  ${REPO_ROOT}/test/progress_sync_retry/host_stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/I18n)
target_compile_definitions(SleepScreenPolicyTest PRIVATE REPO_ROOT="${REPO_ROOT}")
target_link_libraries(SleepScreenPolicyTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(SleepScreenPolicyTest)
