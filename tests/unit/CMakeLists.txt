function(nxemu_unit_test name)
    # Assertions in test programs are checks, including in Release builds.
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

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

# SPIR-V layout of constant buffers. Link the archives themselves: their dependencies would
# bring a second definition of the logging functions.
add_executable(test_spirv_constant_buffers spirv_constant_buffers/layout.cpp)
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
if(WIN32)
    # The plugin DLL exports its module ABI, not its internal C++ classes.
    target_sources(test_loader_metadata PRIVATE
        architecture/module_settings_stub.cpp
        "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/control_metadata.cpp"
        "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs.cpp"
        "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/file_sys/vfs/vfs_vector.cpp")
    target_link_libraries(test_loader_metadata PRIVATE nxemu_test_support yuzu_common)
else()
    target_link_libraries(test_loader_metadata PRIVATE nxemu-loader)
endif()
nxemu_unit_test(test_loader_metadata)

add_executable(test_settings_runtime architecture/settings_runtime.cpp)
target_link_libraries(test_settings_runtime PRIVATE nxemu_core Threads::Threads)
nxemu_unit_test(test_settings_runtime)

add_executable(test_callback_drain architecture/callback_drain.cpp)
target_link_libraries(test_callback_drain PRIVATE nxemu_core Threads::Threads)
nxemu_unit_test(test_callback_drain)

if(TARGET nxemu_ui)
    add_executable(test_architecture_storage architecture/host_storage.cpp)
    target_link_libraries(test_architecture_storage PRIVATE nxemu_ui nxemu_core)
    nxemu_unit_test(test_architecture_storage)
    if(WIN32)
        set_tests_properties(test_architecture_storage PROPERTIES
            ENVIRONMENT "PATH=$<TARGET_FILE_DIR:nxemu>\;$ENV{PATH}")
    endif()
endif()

# Compile the production launch gate against controlled CPU/GPU dependencies.
# Copy without modifications so quoted includes cannot find real Core headers
# beside emu_thread.cpp before consulting the controlled include directory.
configure_file("${CMAKE_SOURCE_DIR}/src/nxemu-os/emu_thread.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/architecture/emu_thread.cpp" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/nxemu-os/emu_thread.h"
    "${CMAKE_CURRENT_BINARY_DIR}/architecture/emu_thread.h" COPYONLY)
add_executable(test_launch_gate architecture/launch_gate.cpp
    architecture/thread_name_stub.cpp "${CMAKE_CURRENT_BINARY_DIR}/architecture/emu_thread.cpp")
target_include_directories(test_launch_gate BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/architecture/stubs"
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/architecture")
target_link_libraries(test_launch_gate PRIVATE Threads::Threads fmt::fmt)
nxemu_unit_test(test_launch_gate)

if(WIN32)
    add_executable(test_module_lifecycle architecture/module_lifecycle.cpp)
    target_link_libraries(test_module_lifecycle PRIVATE nxemu_core dbghelp psapi)
    target_include_directories(test_module_lifecycle PRIVATE "${CMAKE_SOURCE_DIR}/external/vulkan-headers/include")
    add_dependencies(test_module_lifecycle nxemu-loader nxemu-cpu nxemu-video nxemu-os)
    if(MSVC)
        target_compile_options(test_module_lifecycle PRIVATE /UNDEBUG)
    endif()
    add_test(NAME test_module_lifecycle COMMAND test_module_lifecycle "${CMAKE_SOURCE_DIR}"
        $<TARGET_FILE:nxemu-loader> $<TARGET_FILE:nxemu-cpu>
        $<TARGET_FILE:nxemu-video> $<TARGET_FILE:nxemu-os>)
    set_tests_properties(test_module_lifecycle PROPERTIES LABELS "unit;integration" TIMEOUT 120
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:nxemu>\;$ENV{PATH}")
    # Link the production loader objects directly: its DLL exports the module
    # ABI, while this regression needs the internal process registration API.
    add_executable(test_loader_session_isolation architecture/loader_session_isolation.cpp)
    target_link_libraries(test_loader_session_isolation PRIVATE nxemu_loader_objects nxemu_core)
    nxemu_unit_test(test_loader_session_isolation)
    set_tests_properties(test_loader_session_isolation PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:nxemu>\;$ENV{PATH}")
    option(NXEMU_TEST_VULKAN_DISPLAY "Run real-device Vulkan display/session integration tests" OFF)
    if(NXEMU_TEST_VULKAN_DISPLAY)
        add_test(NAME test_vulkan_display_lifecycle COMMAND test_module_lifecycle "${CMAKE_SOURCE_DIR}"
            $<TARGET_FILE:nxemu-loader> $<TARGET_FILE:nxemu-cpu>
            $<TARGET_FILE:nxemu-video> $<TARGET_FILE:nxemu-os> --vulkan)
        set_tests_properties(test_vulkan_display_lifecycle PROPERTIES LABELS "gpu;integration" TIMEOUT 120
            ENVIRONMENT "PATH=$<TARGET_FILE_DIR:nxemu>\;$ENV{PATH}")
    endif()
endif()

add_executable(benchmark_settings architecture/benchmark_settings.cpp)
target_link_libraries(benchmark_settings PRIVATE nxemu_core)

# Optional snapshots prepared by tools/architecture/benchmark_settings.ps1.
set(NXEMU_SETTINGS_BASELINES "" CACHE PATH "SettingsStore source snapshots for A/B benchmarks")
if(NXEMU_SETTINGS_BASELINES)
    foreach(variant master bundle)
        if(EXISTS "${NXEMU_SETTINGS_BASELINES}/${variant}/settings.cpp")
            add_executable(benchmark_settings_${variant} architecture/benchmark_settings.cpp
                "${NXEMU_SETTINGS_BASELINES}/${variant}/settings.cpp")
            target_include_directories(benchmark_settings_${variant} BEFORE PRIVATE
                "${NXEMU_SETTINGS_BASELINES}/${variant}/include")
            target_include_directories(benchmark_settings_${variant} PRIVATE "${CMAKE_SOURCE_DIR}/src")
            target_link_libraries(benchmark_settings_${variant} PRIVATE common)
        endif()
    endforeach()
endif()
