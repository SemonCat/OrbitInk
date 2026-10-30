# Read-loop tests for the REAL lib/KOReaderSync/KOReaderDocumentId.cpp.
#
# Stubs only genuine device dependencies: SD storage (the shared writable in-memory HalStorage,
# with its read-failure injection) and Arduino's MD5Builder, which is replaced by a recorder so
# the test can assert the LENGTHS the read loop hands it -- the thing a negative read corrupts.
add_executable(KOReaderDocumentIdTest
  KOReaderDocumentIdTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderDocumentId.cpp
)

target_include_directories(KOReaderDocumentIdTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/bookorbit_stats/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderDocumentIdTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(KOReaderDocumentIdTest)
