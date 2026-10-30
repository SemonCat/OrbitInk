# BookOrbit owner-scoped library index (src/util/BookOrbitLibraryStore) and the shared atomic
# replacement helper (src/util/AtomicFileReplace.h), over the writable in-memory HalStorage stub
# with its rename/close/write failure injection.
add_executable(BookOrbitLibraryStoreTest
  BookOrbitLibraryStoreTest.cpp
  BookOrbitCatalogCacheTest.cpp
  ${REPO_ROOT}/src/util/BookOrbitLibraryStore.cpp
  ${REPO_ROOT}/src/util/BookOrbitCatalogCache.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/KOReaderSync/BookOrbitOwnerId.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(BookOrbitLibraryStoreTest PRIVATE
  ${REPO_ROOT}/test/bookorbit_stats/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(BookOrbitLibraryStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)
gtest_discover_tests(BookOrbitLibraryStoreTest)
