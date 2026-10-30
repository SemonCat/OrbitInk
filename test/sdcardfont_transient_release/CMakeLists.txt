# Compiles the REAL SdCardFont.cpp / FontCacheManager.cpp / Utf8.cpp against
# small host-only stubs for the ESP32/Arduino/SD-card surface they depend on
# (see host_stubs/). The stub include directory is listed BEFORE the real
# lib/hal and lib/EpdFont directories so <HalStorage.h>/<Logging.h>/
# <FontDecompressor.h> resolve to the stubs instead of the device headers.
#
# FontCacheManager::prewarmCache()'s compressed-flash-font branch calls
# EpdFontFamily::getData(), which is defined out-of-line in EpdFontFamily.cpp.
# That branch is never exercised at runtime by this test (fontMap_ is always
# empty / no FontDecompressor is set), but prewarmCache() itself IS reachable
# now (via FontCacheManager::PrewarmScope::endScanAndPrewarm(), exercised by
# the scan-batching/capacity tests), so the linker keeps the whole function
# and must still resolve every symbol it references, including getData().
# EpdFontFamily.cpp is small and self-contained (depends only on EpdFont,
# already linked below), so it's simplest to just link it in.
add_executable(SdCardFontReleaseTest
  SdCardFontReleaseTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(SdCardFontReleaseTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Utf8
)

target_compile_options(SdCardFontReleaseTest PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/ArduinoStub.h
  -ffunction-sections
  -fdata-sections
)

if(APPLE)
  target_link_options(SdCardFontReleaseTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(SdCardFontReleaseTest PRIVATE -Wl,--gc-sections)
endif()

target_link_libraries(SdCardFontReleaseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SdCardFontReleaseTest)
