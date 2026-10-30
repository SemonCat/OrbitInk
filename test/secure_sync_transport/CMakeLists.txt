# Compile the untouched SDK HTTP parser and owned adapter/client; only the socket and
# Arduino runtime are replaced. No bridge, live server, auth provisioning, or TLS substitution.
add_executable(SecureSyncTransportTest
  SecureSyncTransportTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitSyncClient.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitBookmarks.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SecureSyncTransportTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/test/kosync_docid/host_stubs
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Utf8
)
target_compile_options(SecureSyncTransportTest PRIVATE
  -include ${REPO_ROOT}/test/kosync_protocol/host_stubs/ArduinoStub.h
  -ffunction-sections -fdata-sections
)
if(APPLE)
  target_link_options(SecureSyncTransportTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(SecureSyncTransportTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(SecureSyncTransportTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(SecureSyncTransportTest)

add_executable(X3ClockBoundaryTest X3ClockBoundaryTest.cpp ${REPO_ROOT}/lib/hal/HalClock.cpp)
target_include_directories(X3ClockBoundaryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/lib/hal ${REPO_ROOT}/lib/KOReaderSync)
target_compile_definitions(X3ClockBoundaryTest PRIVATE FREEINK_DEVICE_X3=1 CROSSFADE_CUSTOM_ZH_TW=1)
target_link_libraries(X3ClockBoundaryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(X3ClockBoundaryTest)
