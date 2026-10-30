# BookOrbit as a standalone provider: two host test targets over the REAL sources.
#
#  - BookOrbitClientProtocolTest: BookOrbitSyncClient's own protocol (capability probe,
#    page-stats upload) against a fake transport -- the direct successor to
#    test/bookorbit_stats/BookOrbitProtocolTest.cpp, which tested the same requests back when
#    they lived on KOReaderSyncClient::Session (see docs/bookorbit-feature-matrix.md).
#  - BookOrbitSyncPolicyTest: compiles src/util/BookOrbitStatsSync.cpp -- the real decision-making
#    code -- together with the real client, queue and identity sources, and runs them against a
#    fake transport and an in-memory filesystem. The rules it covers (an unconfirmed server
#    generates a bounded probe but no data loss; an unconfirmed 2xx keeps the queue; another
#    account's events are never uploaded) are properties of how those layers COMPOSE, so a test
#    that stubbed the policy could stay green while the shipped path regressed.
#
# Only genuine device dependencies are stubbed: credentials, settings, persisted app state, the
# RTC, SD storage, and the Arduino ESP globals.
include(FetchContent)
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.2.1
)
FetchContent_MakeAvailable(ArduinoJson)

# ---- BookOrbitSyncClient protocol --------------------------------------------------------------
add_executable(BookOrbitClientProtocolTest
  BookOrbitClientProtocolTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitSyncClient.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitBookmarks.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(BookOrbitClientProtocolTest PRIVATE
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  # MD5Builder.h stub, shared with test/kosync_docid rather than duplicated -- needed by
  # BookOrbitBookmarks.cpp's identity-key digest.
  ${REPO_ROOT}/test/kosync_docid/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Utf8
)
target_compile_options(BookOrbitClientProtocolTest PRIVATE
  -include ${REPO_ROOT}/test/kosync_protocol/host_stubs/ArduinoStub.h
  -ffunction-sections
  -fdata-sections
)
# Session's DEFAULT constructor calls makeDefaultKOSyncTransport() (DefaultKOSyncTransport.cpp),
# which is deliberately NOT linked here because it drags in the real freeink::SecureHttpClient
# and, transitively, Arduino.h/WiFiClient.h/wolfSSL. Every test injects a fake transport instead,
# so dead-code elimination drops that constructor before the linker needs to resolve it.
if(APPLE)
  target_link_options(BookOrbitClientProtocolTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(BookOrbitClientProtocolTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(BookOrbitClientProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)
gtest_discover_tests(BookOrbitClientProtocolTest)

# ---- Policy layer (BookOrbitStatsSync.cpp) -----------------------------------------------------
add_executable(BookOrbitSyncPolicyTest
  BookOrbitSyncPolicyTest.cpp
  BookOrbitStatsWindowTest.cpp
  ${REPO_ROOT}/src/util/BookOrbitStatsSync.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitSyncClient.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitBookmarks.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitStatsQueue.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitStatsDeviceId.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitSyncPolicyTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  # The writable in-memory HalStorage (with its failure injection) is shared with
  # test/bookorbit_stats rather than duplicated -- one stub, one behaviour, for both suites.
  ${REPO_ROOT}/test/bookorbit_stats/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  # MD5Builder.h stub for BookOrbitBookmarks.cpp's identity-key digest -- BookOrbitSyncClient.cpp
  # now links it because it shares a translation unit with the bookmark-exchange methods, even
  # though this target's own tests never call them.
  ${REPO_ROOT}/test/kosync_docid/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
)

target_compile_options(BookOrbitSyncPolicyTest PRIVATE
  # Same force-include the sibling protocol suite uses: supplies the Arduino ESP globals the TLS
  # heap gate reads, before any production header can reference them.
  -include ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/Arduino.h
  -ffunction-sections
  -fdata-sections
)

# BookOrbitOwnerId::derive(baseUrl, username) and bookOrbitFirmwareSha256() are defined in the
# firmware-only mbedTLS translation unit, which cannot link here. This target supplies both from
# the test binary instead (see the bottom of BookOrbitSyncPolicyTest.cpp), so production policy
# code calls exactly the functions it calls on device -- only the digest behind them differs.
if(APPLE)
  target_link_options(BookOrbitSyncPolicyTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(BookOrbitSyncPolicyTest PRIVATE -Wl,--gc-sections)
endif()

target_link_libraries(BookOrbitSyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)
gtest_discover_tests(BookOrbitSyncPolicyTest)
