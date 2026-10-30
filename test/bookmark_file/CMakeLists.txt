# Tests the REAL src/util/BookmarkFile.cpp atomic save()/recovery-on-load() against an in-memory
# HalStorage stub, plus the real lib/Serialization/PersistableStore.cpp (checked writeDocToFile/
# readDocFromFile) and real ArduinoJson (portable, host-buildable) so the JSON round-trip is
# exercised for real, not mocked away.
include(FetchContent)
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.2.1
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(BookmarkFileTest
  BookmarkFileTest.cpp
  ${REPO_ROOT}/src/util/BookmarkFile.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
)

target_include_directories(BookmarkFileTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/test/kosync_protocol/host_stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
)

target_compile_options(BookmarkFileTest PRIVATE
  -ffunction-sections
  -fdata-sections
)

# PersistableStore.cpp's extractPassword() (never called from this test) references
# obfuscation::deobfuscateFromBase64, which this target does not link (see
# host_stubs/ObfuscationUtils.h's own comment) -- dead-stripping drops that unused function before
# the linker needs to resolve the reference. Same technique test/kosync_protocol/CMakeLists.txt
# uses for makeDefaultKOSyncTransport().
if(APPLE)
  target_link_options(BookmarkFileTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(BookmarkFileTest PRIVATE -Wl,--gc-sections)
endif()

target_link_libraries(BookmarkFileTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(BookmarkFileTest)
