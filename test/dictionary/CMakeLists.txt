# Compile the real Dictionary.cpp against small in-memory host seams.  The
# registry resolver and dictzip extractor are defined by DictionaryTest.cpp so
# this target exercises lookup/index logic without pulling in the SD stack.
add_executable(DictionaryTest
  DictionaryTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
)

target_include_directories(DictionaryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/src/util
)

target_link_libraries(DictionaryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryTest)
