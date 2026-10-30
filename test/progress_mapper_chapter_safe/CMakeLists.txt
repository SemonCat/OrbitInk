# Exercises the real ProgressMapper/ChapterXPathResolver implementation with deterministic
# in-memory EPUB and section-cache seams. Hardware, ZIP, SD, and display behavior are stubbed;
# the production mapping code itself is not copied into the fixture.
add_executable(ProgressMapperChapterSafeTest
  ProgressMapperChapterSafeTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(ProgressMapperChapterSafeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(ProgressMapperChapterSafeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressMapperChapterSafeTest)
