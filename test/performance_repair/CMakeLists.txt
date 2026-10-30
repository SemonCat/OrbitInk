add_executable(PerformanceStorageTest
  PerformanceStorageTest.cpp
  ${REPO_ROOT}/src/util/LibraryScanner.cpp
  ${REPO_ROOT}/src/util/LibraryIndex.cpp
  ${REPO_ROOT}/src/util/LibraryIndexBuilder.cpp
  ${REPO_ROOT}/src/util/LibraryIndexDelta.cpp
  ${REPO_ROOT}/src/util/LibraryGrouping.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssCacheLayout.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(PerformanceStorageTest PRIVATE
  host_stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/test/epub_ports/host_stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/JpegToBmpConverter ${REPO_ROOT}/lib/PngToBmpConverter
  ${REPO_ROOT}/lib/ZipFile ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/expat ${REPO_ROOT}/lib/XmlParserUtils
)
target_compile_definitions(PerformanceStorageTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)
target_compile_options(PerformanceStorageTest PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:-include;${REPO_ROOT}/test/epub_ports/host_stubs/Arduino.h>"
  -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(PerformanceStorageTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(PerformanceStorageTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(PerformanceStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PerformanceStorageTest)

add_executable(DownloadCancellationTest
  DownloadCancellationTest.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
  ${REPO_ROOT}/src/network/HttpRedirectPolicy.cpp
)
target_include_directories(DownloadCancellationTest PRIVATE
  download_stubs host_stubs
  ${REPO_ROOT}/test/secure_sync_transport/host_stubs
  ${REPO_ROOT}/test/epub_ports/host_stubs
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include
  ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(DownloadCancellationTest PRIVATE FREEINK_NET_WOLFSSL=1 CROSSPOINT_VERSION="host-test")
target_link_libraries(DownloadCancellationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DownloadCancellationTest)

add_executable(PowerLifecycleTest PowerLifecycleTest.cpp
  ${REPO_ROOT}/lib/hal/HalPowerManager.cpp ${REPO_ROOT}/lib/hal/HalTiltSensor.cpp)
target_include_directories(PowerLifecycleTest PRIVATE power_stubs
  ${REPO_ROOT}/test/epub_ports/host_stubs ${REPO_ROOT}/lib/hal)
target_compile_definitions(PowerLifecycleTest PRIVATE CROSSPOINT_EMULATED=1 BOARD_HAS_PSRAM=0)
target_link_libraries(PowerLifecycleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PowerLifecycleTest)

add_executable(SyncRepairTest SyncRepairTest.cpp
  ${REPO_ROOT}/src/util/ProgressSyncPosition.cpp
  ${REPO_ROOT}/src/util/ProgressSyncNetwork.cpp
  ${REPO_ROOT}/src/util/ProgressSyncDeferred.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp)
target_include_directories(SyncRepairTest PRIVATE sync_stubs
  host_stubs ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/progress_mapper_chapter_safe/host_stubs
  ${REPO_ROOT}/test/epub_ports/host_stubs
  ${REPO_ROOT}/src/util ${REPO_ROOT}/lib/KOReaderSync ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/lib/Utf8)
target_compile_definitions(SyncRepairTest PRIVATE REPO_ROOT="${REPO_ROOT}")
target_link_libraries(SyncRepairTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(SyncRepairTest)

add_executable(FontScratchTest FontScratchTest.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c)
target_include_directories(FontScratchTest PRIVATE power_stubs
  ${REPO_ROOT}/test/epub_ports/host_stubs ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src ${REPO_ROOT}/lib/Utf8)
target_compile_options(FontScratchTest PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include;cstring>"
  -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(FontScratchTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(FontScratchTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(FontScratchTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(FontScratchTest)
