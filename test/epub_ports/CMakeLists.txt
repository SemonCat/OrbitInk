add_executable(EpubPortsTest
  EpubPortsTest.cpp
  HostImageStubs.cpp
  SleepOverlayTest.cpp
  ${REPO_ROOT}/src/util/SleepOverlay.cpp
  ${REPO_ROOT}/src/util/DictHtmlPages.cpp
  ${REPO_ROOT}/src/util/HtmlToPlainText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssCacheLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(EpubPortsTest PRIVATE host_stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/src/util)
target_compile_options(EpubPortsTest PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include;${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/Arduino.h>" -ffunction-sections -fdata-sections)
if(APPLE)
 target_link_options(EpubPortsTest PRIVATE -Wl,-dead_strip)
else()
 target_link_options(EpubPortsTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(EpubPortsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubPortsTest)

target_compile_definitions(EpubPortsTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)
