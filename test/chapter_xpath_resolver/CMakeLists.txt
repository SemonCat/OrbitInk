# Tests the bounded XPath tokenizer (lib/KOReaderSync/ChapterXPathResolverInternal.h) directly
# via its Print write() interface, with real Utf8.cpp/htmlEntities.cpp linked in (both are
# dependency-free pure logic) and only Print.h stubbed (an Arduino-core header not vendored in
# this repo). No Epub/HalStorage/ZIP stack needed -- the tokenizer only cares about bytes in.
add_executable(ChapterXPathResolverTest
  ChapterXPathResolverTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(ChapterXPathResolverTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(ChapterXPathResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterXPathResolverTest)
