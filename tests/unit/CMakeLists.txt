function(nxemu_unit_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

add_executable(test_descriptor_table descriptor_table/batch.cpp)
target_include_directories(test_descriptor_table BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/descriptor_table/stubs" "${CMAKE_SOURCE_DIR}/src")
target_compile_features(test_descriptor_table PRIVATE cxx_std_20)
target_compile_options(test_descriptor_table PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_descriptor_table)

add_executable(test_descriptor_writer descriptor_table/writer.cpp)
target_include_directories(test_descriptor_writer PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_descriptor_writer PRIVATE Vulkan::Headers)
target_compile_features(test_descriptor_writer PRIVATE cxx_std_20)
target_compile_options(test_descriptor_writer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_descriptor_writer)

add_executable(benchmark_descriptor_writer EXCLUDE_FROM_ALL descriptor_table/writer_benchmark.cpp)
target_include_directories(benchmark_descriptor_writer PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(benchmark_descriptor_writer PRIVATE Vulkan::Headers)
target_compile_features(benchmark_descriptor_writer PRIVATE cxx_std_20)

add_executable(benchmark_descriptor_table EXCLUDE_FROM_ALL descriptor_table/benchmark.cpp)
target_include_directories(benchmark_descriptor_table BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/descriptor_table/stubs" "${CMAKE_SOURCE_DIR}/src")
target_compile_features(benchmark_descriptor_table PRIVATE cxx_std_20)

# Host thread state and Host1x syncpoint cancellation, under ASan and UBSan.
add_executable(test_session_lifecycle
    session_lifecycle/test_session_lifecycle.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/syncpoint_manager.cpp")
target_include_directories(test_session_lifecycle PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_session_lifecycle PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(test_session_lifecycle PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_session_lifecycle PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_session_lifecycle)

add_executable(test_query_fences query_fences/fences.cpp)
target_include_directories(test_query_fences BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/query_fences/stubs" "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_query_fences PRIVATE nxemu_test_support Threads::Threads)
target_compile_options(test_query_fences PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_query_fences)

add_executable(test_startup_wait session_lifecycle/startup_wait.cpp)
target_include_directories(test_startup_wait PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_features(test_startup_wait PRIVATE cxx_std_20)
target_link_libraries(test_startup_wait PRIVATE Threads::Threads)
nxemu_unit_test(test_startup_wait)

# SPIR-V layout of constant buffers. Link the archives themselves: their dependencies would
# bring a second definition of the logging functions.
add_executable(test_spirv_constant_buffers spirv_constant_buffers/layout.cpp
    spirv_constant_buffers/runtime_fetch.cpp)
target_compile_options(test_spirv_constant_buffers PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
target_compile_definitions(test_spirv_constant_buffers PRIVATE FMT_HEADER_ONLY)
target_include_directories(test_spirv_constant_buffers PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/externals/SPIRV-Headers/include")
add_dependencies(test_spirv_constant_buffers shader_recompiler sirit fmt common)
target_link_libraries(test_spirv_constant_buffers PRIVATE nxemu_test_support
    $<TARGET_FILE:shader_recompiler> $<TARGET_FILE:sirit> $<TARGET_FILE:fmt> $<TARGET_FILE:common>)
nxemu_unit_test(test_spirv_constant_buffers)

# Application control metadata (NACP) parsing.
add_executable(test_loader_metadata loader_metadata/nacp.cpp)
target_include_directories(test_loader_metadata PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_loader_metadata PRIVATE nxemu-loader)
nxemu_unit_test(test_loader_metadata)

add_subdirectory(shader_profile)

add_executable(test_surface_clip surface_clip/surface_clip.cpp)
target_include_directories(test_surface_clip PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_surface_clip PRIVATE Vulkan::Headers)
nxemu_unit_test(test_surface_clip)

add_executable(test_image_rescaling image_rescaling/image_rescaling.cpp)
target_include_directories(test_image_rescaling PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
add_dependencies(test_image_rescaling video_core yuzu_common common)
target_link_libraries(test_image_rescaling PRIVATE nxemu_test_support
    $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
    fmt bc_decoder stb_dxt)
nxemu_unit_test(test_image_rescaling)

add_executable(test_image_views_and_slices image_views_and_slices/regression.cpp)
target_include_directories(test_image_views_and_slices PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
add_dependencies(test_image_views_and_slices video_core yuzu_common common)
target_link_libraries(test_image_views_and_slices PRIVATE nxemu_test_support
    $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
    fmt bc_decoder stb_dxt)
nxemu_unit_test(test_image_views_and_slices)

add_executable(test_swapchain_present_mode swapchain_present_mode/present_mode.cpp)
target_include_directories(test_swapchain_present_mode PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_swapchain_present_mode PRIVATE Vulkan::Headers)
nxemu_unit_test(test_swapchain_present_mode)

add_test(NAME test_image_slice_copies COMMAND test_image_views_and_slices --copies)
set_tests_properties(test_image_slice_copies PROPERTIES LABELS unit TIMEOUT 120)
