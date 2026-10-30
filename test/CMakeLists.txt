cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(ota_version)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(cjk_traditional_chinese)
add_subdirectory(bmp_coverage_index)
add_subdirectory(zh_tw_custom_profile)
add_subdirectory(kosync_status)
add_subdirectory(sdcardfont_transient_release)
add_subdirectory(kosync_release_ordering)
add_subdirectory(chapter_xpath_resolver)
add_subdirectory(kosync_protocol)
add_subdirectory(progress_debounce)
add_subdirectory(refresh_eligibility)
add_subdirectory(quick_resume_eligibility)
add_subdirectory(image_zip_hardening)
add_subdirectory(bookorbit_stats)
add_subdirectory(bookorbit_sync)
add_subdirectory(bookorbit_migration)
add_subdirectory(bookorbit_catalog)
add_subdirectory(bookorbit_library)
add_subdirectory(kosync_docid)
add_subdirectory(progress_pull_policy)
add_subdirectory(bookorbit_bookmark_sync)
add_subdirectory(http_redirect_policy)
add_subdirectory(bookmark_file)
add_subdirectory(sync_web_settings)
add_subdirectory(reader_build_window)
add_subdirectory(text_wrap)
add_subdirectory(library_index_delta)
add_subdirectory(css_cache_layout)
add_subdirectory(css_loader_oom_guard)
add_subdirectory(idle_baseline_latch)
add_subdirectory(dashboard_home_model)
add_subdirectory(dashboard_layout)
add_subdirectory(journal_layout)
add_subdirectory(bookorbit_setup)
add_subdirectory(dictionary)

add_subdirectory(epub_ports)

add_subdirectory(progress_sync_retry)
add_subdirectory(progress_mapper_chapter_safe)
add_subdirectory(secure_sync_transport)
add_subdirectory(firmware_update)
add_subdirectory(performance_repair)
