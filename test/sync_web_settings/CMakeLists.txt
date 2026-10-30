FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.2.1
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(SyncSettingsWebPolicyTest
  SyncSettingsWebPolicyTest.cpp
  ${REPO_ROOT}/src/network/SyncSettingsWebPolicy.cpp
)

target_include_directories(SyncSettingsWebPolicyTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(SyncSettingsWebPolicyTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(SyncSettingsWebPolicyTest)
