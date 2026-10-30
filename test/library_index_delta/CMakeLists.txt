# Tests the carry-over half of LibraryIndexBuilder::begin() in isolation -- see
# src/util/LibraryIndexDelta.h. No SD storage, no EPUB parsing.
add_executable(LibraryIndexDeltaTest
  LibraryIndexDeltaTest.cpp
  ${REPO_ROOT}/src/util/LibraryIndexDelta.cpp
)

target_include_directories(LibraryIndexDeltaTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
)

target_link_libraries(LibraryIndexDeltaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryIndexDeltaTest)
