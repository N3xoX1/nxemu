# Header-level regressions for descriptor invalidation and sparse slot iteration.
add_executable(test_gpu_cache_hotpaths cache_hotpaths.cpp)
target_include_directories(test_gpu_cache_hotpaths BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${NXEMU_FAST_TEST_SOURCE_DIR}")
target_compile_options(test_gpu_cache_hotpaths PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_fast_test(test_gpu_cache_hotpaths)
