add_custom_target(nxemu_unit_tests)
add_custom_target(nxemu_linux_unit_tests)

function(nxemu_unit_test name)
    add_dependencies(nxemu_unit_tests ${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 120)
    if("LINUX" IN_LIST ARGN)
        add_dependencies(nxemu_linux_unit_tests ${name})
        set_property(TEST ${name} APPEND PROPERTY LABELS linux_ci)
    endif()
endfunction()

add_executable(test_lot1_audio eden_lot1/audio.cpp)
target_include_directories(test_lot1_audio PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-os" "${CMAKE_SOURCE_DIR}/external/boost"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_lot1_audio PRIVATE FMT_HEADER_ONLY)
nxemu_unit_test(test_lot1_audio LINUX)

# Audio callbacks must not outlive their manager; circular sinks own no upsampler.
add_executable(test_audio_manager_lifetime audio_lifetime/manager.cpp
    ${CMAKE_SOURCE_DIR}/src/yuzu_audio_core/audio_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/yuzu_audio_core/audio_event.cpp)
add_executable(test_circular_sink_cleanup audio_lifetime/circular_sink.cpp)
foreach(_test test_audio_manager_lifetime test_circular_sink_cleanup)
    target_include_directories(${_test} PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/nxemu-os" "${CMAKE_SOURCE_DIR}/external/boost"
        "${CMAKE_SOURCE_DIR}/external/fmt/include")
    target_compile_definitions(${_test} PRIVATE FMT_HEADER_ONLY)
    target_link_libraries(${_test} PRIVATE nxemu_test_support)
    if(NOT MSVC)
        target_compile_options(${_test} PRIVATE
            -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${_test} PRIVATE -fsanitize=address,undefined)
    endif()
    nxemu_unit_test(${_test})
endforeach()

add_executable(test_lot1_lifecycle eden_lot1/lifecycle.cpp)
target_include_directories(test_lot1_lifecycle PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-os" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_lot1_lifecycle PRIVATE nxemu_test_support fmt::fmt)
nxemu_unit_test(test_lot1_lifecycle LINUX)

if(ARCHITECTURE_x86_64)
    add_executable(test_lot1_tsc eden_lot1/tsc.cpp
        "${CMAKE_SOURCE_DIR}/src/yuzu_common/x64/rdtsc.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_common/steady_clock.cpp")
    target_include_directories(test_lot1_tsc PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_lot1_tsc PRIVATE Threads::Threads)
    nxemu_unit_test(test_lot1_tsc LINUX)
endif()

if(ARCHITECTURE_x86_64)
    # Exercise the JIT objects shipped in the CPU module, including its Release flags.
    add_executable(test_dynarmic_regalloc dynarmic_regalloc/jit.cpp
        $<TARGET_OBJECTS:nxemu-cpu>)
    target_include_directories(test_dynarmic_regalloc PRIVATE "${CMAKE_SOURCE_DIR}/src/nxemu-cpu")
    target_link_libraries(test_dynarmic_regalloc PRIVATE common yuzu_common fmt merry::mcl Zydis)
    nxemu_unit_test(test_dynarmic_regalloc LINUX)
endif()

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
    add_executable(test_strided_copy_gpu strided_handles/gpu_copy.cpp
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_settings.cpp"
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_enum_strings.cpp")
    target_include_directories(test_strided_copy_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
        "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
    target_link_libraries(test_strided_copy_gpu PRIVATE nxemu_test_support video_core
        Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS})
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
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/engine_upload.cpp"
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
nxemu_unit_test(test_shader_constant_folding LINUX)

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
add_executable(test_loader_metadata loader_metadata/nacp.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/control_metadata.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs_vector.cpp")
target_include_directories(test_loader_metadata PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_loader_metadata PRIVATE nxemu_test_support yuzu_common common fmt::fmt)
nxemu_unit_test(test_loader_metadata)

# Patch sets (.nxpatch): in-place and cave sites, relocation of PC-relative cave words, whole-set rejection.
add_executable(test_patch_sets patch_sets/patch_sets.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/patch_sets.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs_vector.cpp")
target_include_directories(test_patch_sets PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_options(test_patch_sets PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
target_link_libraries(test_patch_sets PRIVATE nxemu_test_support yuzu_common common fmt::fmt)
nxemu_unit_test(test_patch_sets)

# Hash of the enabled mods' ExeFS patches and cheats, which sets a modded run apart from the game.
add_executable(test_code_mods code_mods/code_mods.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/patch_sets.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs_vector.cpp")
target_include_directories(test_code_mods PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_options(test_code_mods PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
target_link_libraries(test_code_mods PRIVATE nxemu_test_support yuzu_common common fmt::fmt)
nxemu_unit_test(test_code_mods)

# Loose ExeFS discovery and safe pruning of large extracted RomFS directories.
add_executable(test_loose_romfs_paths loose_romfs_paths/paths.cpp)
target_include_directories(test_loose_romfs_paths PRIVATE "${CMAKE_SOURCE_DIR}/src/nxemu-loader")
target_compile_features(test_loose_romfs_paths PRIVATE cxx_std_20)
target_compile_options(test_loose_romfs_paths PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_loose_romfs_paths)

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
target_link_libraries(test_nce_written_ranges PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(test_nce_written_ranges PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_nce_written_ranges PRIVATE -fsanitize=address,undefined)
    add_executable(test_gpu_dirty_memory_tsan nce_written_ranges/written_ranges.cpp)
    target_include_directories(test_gpu_dirty_memory_tsan PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_options(test_gpu_dirty_memory_tsan PRIVATE -O1 -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_gpu_dirty_memory_tsan PRIVATE -fsanitize=thread)
    target_link_libraries(test_gpu_dirty_memory_tsan PRIVATE Threads::Threads)
    nxemu_unit_test(test_gpu_dirty_memory_tsan LINUX)
endif()
nxemu_unit_test(test_nce_written_ranges LINUX)

add_executable(test_nce_store_thunk nce_store_thunk/store_thunk.cpp)
target_include_directories(test_nce_store_thunk PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_nce_store_thunk PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_unit_test(test_nce_store_thunk)

add_executable(test_storage_pointer_tracking
    storage_pointer_tracking/loops.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp")
# Exercise multi-chunk pools with consistent ASan container annotations in the
# fixture and the IR code that allocates their instructions.
foreach(source basic_block ir_emitter microinstruction)
    target_sources(test_storage_pointer_tracking PRIVATE
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/frontend/ir/${source}.cpp")
endforeach()
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
foreach(_part update notify format)
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

option(NXEMU_BUILD_VTG_GPU_TESTS "Build the manual headless Vulkan test of indirect draws running as compute" OFF)
if(NXEMU_BUILD_VTG_GPU_TESTS)
    add_executable(test_vtg_indirect_gpu vtg_indirect/vtg_indirect_gpu.cpp
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_settings.cpp"
        "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_enum_strings.cpp")
    target_include_directories(test_vtg_indirect_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
        "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
    add_dependencies(test_vtg_indirect_gpu video_core yuzu_common common shader_recompiler sirit)
    target_link_libraries(test_vtg_indirect_gpu PRIVATE nxemu_test_support video_core
        Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS})
endif()

# Execute the production texture pass, including wrapped U32 guest addresses.
add_executable(test_strided_handles strided_handles/handles.cpp)
target_include_directories(test_strided_handles PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_strided_handles PRIVATE FMT_HEADER_ONLY)
add_dependencies(test_strided_handles shader_recompiler sirit fmt common)
target_link_libraries(test_strided_handles PRIVATE nxemu_test_support
    $<TARGET_FILE:shader_recompiler> $<TARGET_FILE:sirit> $<TARGET_FILE:fmt> $<TARGET_FILE:common>)
nxemu_unit_test(test_strided_handles)

# Differential tests execute the production DMA and Maxwell register methods.
set(_gpu_commands_generated "${CMAKE_CURRENT_BINARY_DIR}/gpu_commands")
add_custom_command(
    OUTPUT "${_gpu_commands_generated}/engine.inc" "${_gpu_commands_generated}/dma.inc"
        "${_gpu_commands_generated}/baseline.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gpu_commands/extract.py"
        "${CMAKE_SOURCE_DIR}" "${_gpu_commands_generated}"
    DEPENDS gpu_commands/extract.py gpu_commands/baseline.inc
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/engines/maxwell_3d.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/dma_pusher.cpp"
    VERBATIM)
add_executable(test_gpu_commands gpu_commands/commands.cpp
    "${_gpu_commands_generated}/engine.inc" "${_gpu_commands_generated}/dma.inc"
    "${_gpu_commands_generated}/baseline.inc")
target_include_directories(test_gpu_commands PRIVATE "${_gpu_commands_generated}"
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/boost"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_gpu_commands PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_gpu_commands PRIVATE nxemu_test_support)
if(NOT MSVC)
    target_compile_options(test_gpu_commands PRIVATE
        -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(test_gpu_commands PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_gpu_commands)

set(_texture_hotpaths_generated "${CMAKE_CURRENT_BINARY_DIR}/texture_hotpaths")
add_custom_command(
    OUTPUT "${_texture_hotpaths_generated}/regions.inc" "${_texture_hotpaths_generated}/pages.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/texture_hotpaths/extract.py"
        "${CMAKE_SOURCE_DIR}" "${_texture_hotpaths_generated}"
    DEPENDS texture_hotpaths/extract.py
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/texture_cache/texture_cache.h"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/texture_cache/texture_cache_base.h"
    VERBATIM)
add_executable(test_texture_regions texture_hotpaths/regions.cpp
    "${_texture_hotpaths_generated}/regions.inc" "${_texture_hotpaths_generated}/pages.inc")
target_include_directories(test_texture_regions PRIVATE "${_texture_hotpaths_generated}"
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/boost"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_texture_regions PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_texture_regions PRIVATE nxemu_test_support)
if(NOT MSVC)
    target_compile_options(test_texture_regions PRIVATE
        -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(test_texture_regions PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_texture_regions)

add_executable(test_mapping_continuity mapping_continuity/mapping.cpp)
target_include_directories(test_mapping_continuity PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/boost")
add_dependencies(test_mapping_continuity video_core yuzu_common common)
target_link_libraries(test_mapping_continuity PRIVATE nxemu_test_support
    $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
    fmt bc_decoder stb_dxt)
nxemu_unit_test(test_mapping_continuity)

# Each test suite owns its build and CTest registration.
file(GLOB unit_suites RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(suite IN LISTS unit_suites)
    get_filename_component(suite_directory "${suite}" DIRECTORY)
    add_subdirectory("${suite_directory}")
endforeach()
