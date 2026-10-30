# Host tests for the EPUB image/ZIP hardening fixes:
#   - ImageToFramebufferDecoder::validateImageDimensions (64-bit overflow-safe 3MP gate)
#   - ImageMarginClamp.h (full-viewport image + CSS top margin clamping)
#   - ZipFile.cpp's zipFillCallback negative-read guard (real ZipFile.cpp + InflateStream.cpp +
#     the vendored miniz tinfl, against a synthetic in-memory ZIP)
#
# Host stubs (host_stubs/) replace only genuine device dependencies (HalStorage/HalFile's real
# SdFat backing, device Logging.h); everything else under test is the REAL production source,
# compiled directly -- see each .cpp's own header comment for what it links and why.
add_executable(ImageZipHardeningTest
  ImageDimsValidationTest.cpp
  ImageMarginClampTest.cpp
  ZipNegativeReadTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
)

target_include_directories(ImageZipHardeningTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(ImageZipHardeningTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# AddressSanitizer, for this target only: ZipNegativeReadTest's whole point is proving the
# negative-read guard prevents an out-of-bounds read inside InflateStream's fixed-size read
# buffer (see zipFillCallback's comment in ZipFile.cpp). A plain host build can't reliably tell
# "read garbage, then happened to error out anyway" apart from "actually stayed in bounds" --
# ASan makes an out-of-bounds byte access a deterministic, immediate failure regardless of what
# garbage is on the other side of the buffer.
target_compile_options(ImageZipHardeningTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(ImageZipHardeningTest PRIVATE -fsanitize=address,undefined)

gtest_discover_tests(ImageZipHardeningTest)
