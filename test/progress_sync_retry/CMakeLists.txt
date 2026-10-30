add_executable(ProgressSyncRetryTest
  ProgressSyncRetryTest.cpp
  ${REPO_ROOT}/src/CrossPointState.cpp
)
target_include_directories(ProgressSyncRetryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/KOReaderSync
)
target_compile_definitions(ProgressSyncRetryTest PRIVATE REPO_ROOT="${REPO_ROOT}")
target_link_libraries(ProgressSyncRetryTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(ProgressSyncRetryTest)
