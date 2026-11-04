# Header-level regressions for descriptor invalidation and sparse slot iteration.
add_executable(test_gpu_cache_hotpaths cache_hotpaths.cpp)
target_include_directories(test_gpu_cache_hotpaths BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_SOURCE_DIR}/src")
target_compile_features(test_gpu_cache_hotpaths PRIVATE cxx_std_20)
target_compile_options(test_gpu_cache_hotpaths PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_gpu_cache_hotpaths)
