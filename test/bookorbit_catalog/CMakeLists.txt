# BookOrbitCatalogClient host tests, against the REAL lib/KOReaderSync/BookOrbitCatalogClient.cpp.
#
# Two things are under test, deliberately kept in one binary since both exercise the same Session
# type: the JSON protocol methods (root sections, book listing/paging, facet listing, book detail
# with EPUB filtering) against a fake KOSyncTransport, and downloadFile()'s SD-side atomicity
# (ownership-marked temp+backup+promote, short write, close/flush failure, rename failure,
# stale-backup recovery, unowned-sidecar preservation, and cleanup-I/O refusal) against an INJECTED
# FileFetcher plus the same in-memory, failure-injectable HalStorage stub BookOrbitStatsQueue's own
# tests use -- one stub, one behaviour, for every BookOrbit suite that needs a writable fake
# filesystem.
include(FetchContent)
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.2.1
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(BookOrbitCatalogClientTest
  BookOrbitCatalogClientTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitCatalogClient.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(BookOrbitCatalogClientTest PRIVATE
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  # The writable in-memory HalStorage (with rename/close/write failure injection) is shared with
  # test/bookorbit_stats rather than duplicated.
  ${REPO_ROOT}/test/bookorbit_stats/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Utf8
)
target_compile_options(BookOrbitCatalogClientTest PRIVATE
  -include ${REPO_ROOT}/test/kosync_protocol/host_stubs/ArduinoStub.h
  -ffunction-sections
  -fdata-sections
)
# Session's DEFAULT constructor calls makeDefaultKOSyncTransport() (DefaultKOSyncTransport.cpp)
# and wires bookOrbitDefaultFileFetch() (src/network/BookOrbitCatalogDownload.cpp, which in turn
# calls HttpDownloader::downloadToFile()) -- none of that is linked here because it drags in
# ESP-IDF/wolfSSL/Arduino headers this suite has no stand-ins for. Every test uses the explicit
# (transport, fileFetcher) constructor instead, so dead-code elimination drops the default
# constructor, and everything it alone references, before the linker needs to resolve any of it.
if(APPLE)
  target_link_options(BookOrbitCatalogClientTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(BookOrbitCatalogClientTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(BookOrbitCatalogClientTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)
gtest_discover_tests(BookOrbitCatalogClientTest)
