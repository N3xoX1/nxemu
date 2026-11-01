function(nxemu_unit_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

# Real Fermi2D, software blitter, GPU page tables and device-memory mappings.
# The fixture models cache notifications; GPU cache integration needs a renderer.
add_executable(test_fast_copy fast_copy/fast_copy.cpp)
target_include_directories(test_fast_copy PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/boost")
add_dependencies(test_fast_copy video_core yuzu_common common)
target_link_libraries(test_fast_copy PRIVATE nxemu_test_support
    $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
    fmt bc_decoder stb_dxt)
nxemu_unit_test(test_fast_copy)

option(NXEMU_BUILD_COPY_GPU_TESTS "Build the manual headless Vulkan copy integration test" OFF)
if(NXEMU_BUILD_COPY_GPU_TESTS)
    add_executable(test_fast_copy_gpu fast_copy/gpu_copy.cpp
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_settings.cpp"
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_enum_strings.cpp")
    target_include_directories(test_fast_copy_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
        "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
    add_dependencies(test_fast_copy_gpu video_core yuzu_common common shader_recompiler sirit)
    target_link_libraries(test_fast_copy_gpu PRIVATE nxemu_test_support video_core
        Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS})
endif()

if(NOT MSVC)
    # Instrument the production paths themselves, not just the test executable.
    add_executable(test_fast_copy_sanitized fast_copy/fast_copy.cpp
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/fermi_2d.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/maxwell_dma.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/sw_blitter/blitter.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/textures/decoders.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/memory_manager.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/gpu_device_memory_manager.cpp")
    target_include_directories(test_fast_copy_sanitized PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
        "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
    add_dependencies(test_fast_copy_sanitized video_core yuzu_common common)
    target_link_libraries(test_fast_copy_sanitized PRIVATE nxemu_test_support
        $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
        fmt bc_decoder stb_dxt)
    target_compile_options(test_fast_copy_sanitized PRIVATE
        -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(test_fast_copy_sanitized PRIVATE -fsanitize=address,undefined)
    nxemu_unit_test(test_fast_copy_sanitized)
endif()

# Compile the production integer folding pass and IR with sanitizers as well as the test.
add_executable(test_shader_constant_folding shader_constant_folding/folding.cpp)
foreach(source attribute basic_block condition flow_test ir_emitter microinstruction opcodes patch program type value)
    target_sources(test_shader_constant_folding PRIVATE
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/frontend/ir/${source}.cpp")
endforeach()
foreach(source constant_propagation_pass dead_code_elimination_pass identity_removal_pass verification_pass)
    target_sources(test_shader_constant_folding PRIVATE
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/ir_opt/${source}.cpp")
endforeach()
target_include_directories(test_shader_constant_folding PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_shader_constant_folding PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_shader_constant_folding PRIVATE nxemu_test_support)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(test_shader_constant_folding PRIVATE -fbracket-depth=1024)
endif()
if(NOT MSVC)
    target_compile_options(test_shader_constant_folding PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_shader_constant_folding PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_shader_constant_folding)

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

# Game launch on the emulation thread: release, failed preparation, cancellation and shutdown.
add_executable(test_launch_gate session_lifecycle/launch_gate.cpp)
target_include_directories(test_launch_gate PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_features(test_launch_gate PRIVATE cxx_std_20)
target_link_libraries(test_launch_gate PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(test_launch_gate PRIVATE -O1 -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_launch_gate PRIVATE -fsanitize=thread)
endif()
nxemu_unit_test(test_launch_gate)

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
target_compile_options(test_spirv_constant_buffers PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
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

add_executable(test_nce_written_ranges nce_written_ranges/written_ranges.cpp)
target_include_directories(test_nce_written_ranges PRIVATE "${CMAKE_SOURCE_DIR}/src")
nxemu_unit_test(test_nce_written_ranges)

add_executable(test_nce_store_thunk nce_store_thunk/store_thunk.cpp)
target_include_directories(test_nce_store_thunk PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_nce_store_thunk PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_nce_store_thunk)

add_executable(test_storage_pointer_tracking
    storage_pointer_tracking/loops.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp")
target_include_directories(test_storage_pointer_tracking PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_storage_pointer_tracking PRIVATE shader_recompiler yuzu_common)
if(NOT MSVC)
    target_compile_options(test_storage_pointer_tracking PRIVATE
        -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(test_storage_pointer_tracking PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_storage_pointer_tracking)

add_executable(test_empty_storage_warnings storage_pointer_tracking/empty_warnings.cpp)
target_include_directories(test_empty_storage_warnings PRIVATE "${CMAKE_SOURCE_DIR}/src")
nxemu_unit_test(test_empty_storage_warnings)

add_executable(test_surface_atomic_arrays surface_atomic_arrays/coordinates.cpp)
target_include_directories(test_surface_atomic_arrays PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_surface_atomic_arrays PRIVATE shader_recompiler yuzu_common)
nxemu_unit_test(test_surface_atomic_arrays)

add_executable(test_alpha_to_coverage_dither alpha_to_coverage_dither/dither.cpp)
target_include_directories(test_alpha_to_coverage_dither PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/externals/SPIRV-Headers/include")
add_dependencies(test_alpha_to_coverage_dither shader_recompiler sirit fmt common)
target_link_libraries(test_alpha_to_coverage_dither PRIVATE nxemu_test_support
    $<TARGET_FILE:shader_recompiler> $<TARGET_FILE:sirit> $<TARGET_FILE:fmt> $<TARGET_FILE:common>)
nxemu_unit_test(test_alpha_to_coverage_dither)

# Compile unchanged conditional-rendering method bodies with fake GPU dependencies.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_vertex_input_generated "${CMAKE_CURRENT_BINARY_DIR}/vertex_input")
set(_vertex_input_includes)
foreach(_part update notify format states)
    list(APPEND _vertex_input_includes "${_vertex_input_generated}/${_part}.inc")
endforeach()
add_custom_command(
    OUTPUT ${_vertex_input_includes}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vertex_input/extract.py"
        "${CMAKE_SOURCE_DIR}" "${_vertex_input_generated}"
    DEPENDS vertex_input/extract.py
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/vk_buffer_cache.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/maxwell_to_vk.cpp"
    VERBATIM)
add_executable(test_vertex_input vertex_input/vertex_input.cpp ${_vertex_input_includes})
target_include_directories(test_vertex_input PRIVATE "${_vertex_input_generated}"
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/boost"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_vertex_input PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_vertex_input PRIVATE nxemu_test_support Vulkan::Headers)
if(NOT MSVC)
    target_compile_options(test_vertex_input PRIVATE
        -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(test_vertex_input PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_vertex_input)
set(NXEMU_VERTEX_INPUT_REFERENCE_PATCH "" CACHE FILEPATH "Reviewed Lot1 patch for host comparison")
set(NXEMU_VERTEX_INPUT_BASELINE "HEAD" CACHE STRING "Git ref before the vertex-input fix")
if(NXEMU_VERTEX_INPUT_REFERENCE_PATCH)
    set(_vertex_input_compare)
    foreach(_part original supplied consume_supplied notify_supplied)
        list(APPEND _vertex_input_compare "${_vertex_input_generated}/${_part}.inc")
    endforeach()
    add_custom_command(
        OUTPUT ${_vertex_input_compare}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vertex_input/prepare_compare.py"
            "${CMAKE_SOURCE_DIR}" "${_vertex_input_generated}"
            "${NXEMU_VERTEX_INPUT_REFERENCE_PATCH}" "${NXEMU_VERTEX_INPUT_BASELINE}"
        DEPENDS vertex_input/prepare_compare.py vertex_input/extract.py
            "${NXEMU_VERTEX_INPUT_REFERENCE_PATCH}"
        VERBATIM)
    add_executable(bench_vertex_input vertex_input/vertex_input.cpp
        ${_vertex_input_includes} ${_vertex_input_compare})
    target_include_directories(bench_vertex_input PRIVATE "${_vertex_input_generated}"
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/boost"
        "${CMAKE_SOURCE_DIR}/external/fmt/include")
    target_compile_definitions(bench_vertex_input PRIVATE FMT_HEADER_ONLY VERTEX_INPUT_COMPARE)
    target_link_libraries(bench_vertex_input PRIVATE nxemu_test_support Vulkan::Headers)
endif()
set(_conditional_generated "${CMAKE_CURRENT_BINARY_DIR}/conditional_rendering")
add_custom_command(
    OUTPUT "${_conditional_generated}/engine.inc" "${_conditional_generated}/runtime.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/conditional_rendering/extract.py"
        "${CMAKE_SOURCE_DIR}" "${_conditional_generated}"
    DEPENDS conditional_rendering/extract.py
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/maxwell_3d.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/dma_pusher.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp"
    VERBATIM)
foreach(_part engine runtime)
    add_executable(test_conditional_${_part} conditional_rendering/${_part}.cpp
        "${_conditional_generated}/${_part}.inc")
    target_include_directories(test_conditional_${_part} PRIVATE "${_conditional_generated}"
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/boost"
        "${CMAKE_SOURCE_DIR}/external/fmt/include")
    target_compile_definitions(test_conditional_${_part} PRIVATE FMT_HEADER_ONLY)
    target_link_libraries(test_conditional_${_part} PRIVATE nxemu_test_support Vulkan::Headers)
    if(NOT MSVC)
        target_compile_options(test_conditional_${_part} PRIVATE
            -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(test_conditional_${_part} PRIVATE -fsanitize=address,undefined)
    endif()
    nxemu_unit_test(test_conditional_${_part})
endforeach()
