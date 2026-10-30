# Tests the REAL lib/KOReaderSync/KOReaderSyncClient.cpp protocol logic (JSON building/parsing,
# HTTP-status-to-Error categorization, primary/alternate labeling, connection-reuse routing)
# against a fake, in-memory KOSyncTransport -- no real network/WiFi/TLS stack. Host stubs replace
# only genuine device/vendor dependencies (KOReaderCredentialStore's real SD persistence,
# Arduino's ESP/base64/String globals, device logging); ArduinoJson is pulled in for real via
# FetchContent since it is itself a portable, host-buildable library.
include(FetchContent)
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.2.1
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(KOSyncProtocolTest
  KOSyncProtocolTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncClient.cpp
)

target_include_directories(KOSyncProtocolTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
)

target_compile_options(KOSyncProtocolTest PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/ArduinoStub.h
  -ffunction-sections
  -fdata-sections
)

# KOReaderSyncClient::Session's DEFAULT constructor calls makeDefaultKOSyncTransport()
# (DefaultKOSyncTransport.cpp), which is deliberately NOT linked into this test target -- it
# pulls in the real freeink::SecureHttpClient and, transitively, Arduino.h/WiFiClient.h/wolfSSL,
# exactly what this test exists to avoid needing. None of these tests call that constructor (all
# inject a fake transport via the other constructor instead), so dead-code elimination drops the
# unused default constructor's object code -- and with it the otherwise-unresolved reference to
# makeDefaultKOSyncTransport() -- before the linker ever needs to resolve it.
if(APPLE)
  target_link_options(KOSyncProtocolTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(KOSyncProtocolTest PRIVATE -Wl,--gc-sections)
endif()

target_link_libraries(KOSyncProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(KOSyncProtocolTest)
