# Real mbedTLS (same 3.6 line the ESP-IDF 5.5 framework ships) so the ECDSA P-256 verify and SHA256
# exercised here are the exact library calls the firmware makes. Crypto library only. The archive is
# hash-pinned and the fetch bounded; an offline checkout can point CMake at a local copy with
# -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<dir>.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2
  URL_HASH SHA256=4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
)
FetchContent_MakeAvailable(mbedtls)

if(NOT TARGET ArduinoJson)
  FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.2.1
  )
  FetchContent_MakeAvailable(ArduinoJson)
endif()

add_executable(FirmwareUpdateTest
  EspImageValidatorTest.cpp
  FirmwareUploadSessionTest.cpp
  OtaManifestTest.cpp
  OtaHttpTest.cpp
  ${REPO_ROOT}/lib/FirmwareUpdate/EspImageValidator.cpp
  ${REPO_ROOT}/lib/FirmwareUpdate/FirmwareUploadSession.cpp
  ${REPO_ROOT}/lib/FirmwareUpdate/OtaManifest.cpp
  ${REPO_ROOT}/lib/FirmwareUpdate/OtaSignature.cpp
  ${REPO_ROOT}/lib/FirmwareUpdate/OtaHttp.cpp
)

target_include_directories(FirmwareUpdateTest PRIVATE
  ${REPO_ROOT}/lib/FirmwareUpdate
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(FirmwareUpdateTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  MbedTLS::mbedcrypto
  GTest::gtest_main
)

gtest_discover_tests(FirmwareUpdateTest)
