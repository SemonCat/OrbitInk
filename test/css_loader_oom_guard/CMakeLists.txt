# Regression test for the loadFromCache() unguarded-allocation abort (see harness_main.cpp).
# Deliberately a standalone executable exercised via multiple `add_test()` invocations (one
# process per scenario) rather than a GTest binary: this target needs its own global operator new
# override and its own -fno-exceptions build of the real CSS parser sources, and running each
# budget scenario in a fresh process avoids a process-wide allocator override interacting with a
# shared test runner's own internal allocations across cases.
add_executable(CssLoaderOomGuardHarness
  harness_main.cpp
  alloc_budget.cpp
  throw_helper.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssCacheLayout.cpp
)

target_include_directories(CssLoaderOomGuardHarness PRIVATE
  host_stubs
  # Reuse the existing HalStorage.h/Logging.h host stubs from epub_ports rather than duplicating
  # them -- this target only adds its own Arduino.h (live allocation-budget counter) on top.
  ${REPO_ROOT}/test/epub_ports/host_stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Memory
)

# Compile the real parser (and the test/allocator glue -- everything except throw_helper.cpp) with
# -fno-exceptions, matching platformio.ini's actual firmware build flags, so an unguarded
# allocation here throws uncaught into code with no landing pads, exactly like the real device.
# throw_helper.cpp stands in for the precompiled runtime library and is deliberately left out --
# it must keep exceptions enabled to have somewhere to throw std::bad_alloc *from*.
set_source_files_properties(
  harness_main.cpp
  alloc_budget.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssCacheLayout.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions"
)

target_link_libraries(CssLoaderOomGuardHarness PRIVATE
  crosspoint_test_common
)

# Discover and fail every nothrow allocation ordinal used by a successful
# real-book-shaped cold/warm load. This covers initial allocation and mid-load
# growth without freezing brittle ordinal bounds into CMake.
add_test(NAME CssLoaderOomGuard.ColdAllocationSweep
         COMMAND CssLoaderOomGuardHarness cold-sweep)
add_test(NAME CssLoaderOomGuard.WarmAllocationSweep
         COMMAND CssLoaderOomGuardHarness warm-sweep)

add_test(NAME CssLoaderOomGuard.RealBookShape
         COMMAND CssLoaderOomGuardHarness book-shape)
add_test(NAME CssLoaderOomGuard.ReadFailureAfterRule
         COMMAND CssLoaderOomGuardHarness read-failure)
add_test(NAME CssLoaderOomGuard.CascadeAndHiddenSemantics
         COMMAND CssLoaderOomGuardHarness semantics)
add_test(NAME CssLoaderOomGuard.CacheIntegrityAndVersion
         COMMAND CssLoaderOomGuardHarness cache-integrity)
