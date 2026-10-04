cmake_minimum_required(VERSION 3.22)

# Match the minimum supported by the bundled MoltenVK runtime, instead of
# silently targeting the macOS version of the developer's machine.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(nxemu)

if(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.0")
        message(FATAL_ERROR "The bundled MoltenVK runtime requires macOS 12.0 or newer")
    endif()
    enable_language(OBJC OBJCXX)
endif()

find_package(Threads REQUIRED)

if(APPLE)
    option(ENABLE_SDL2 "Enable SDL game controller input" ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
       "Enable performance capture instrumentation" OFF)
if(NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION)
    add_compile_definitions(NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION=1)
else()
    add_compile_definitions(NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION=0)
endif()

# ============================================================
# Architecture detection
# ============================================================
set(NXEMU_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _nxemu_arch_count)
    if(_nxemu_arch_count GREATER 1)
        message(FATAL_ERROR "Build each macOS architecture separately: the JIT backend is architecture-specific")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 NXEMU_TARGET_PROCESSOR)
endif()
if(NXEMU_TARGET_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(ARCHITECTURE_arm64 TRUE)
    set(ARCHITECTURE "arm64")
    set(NXEMU_PLATFORM_DIR "arm64")
    add_compile_definitions(ARCHITECTURE_arm64=1)
elseif(NXEMU_TARGET_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_definitions(ARCHITECTURE_x86_64=1)
    set(ARCHITECTURE_x86_64 TRUE)
    set(ARCHITECTURE "x86_64")
    set(NXEMU_PLATFORM_DIR "x64")
else()
    set(NXEMU_PLATFORM_DIR "${NXEMU_TARGET_PROCESSOR}")
endif()

# CMake modules
# ============================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/src/cmake/modules")
include("${CMAKE_SOURCE_DIR}/src/cmake/modules/create_target_directory_groups.cmake")
include(nxemu_output_directories)

# ============================================================
# Version
# ============================================================
set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-core/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-core/version.h")
include(generate_version_header)
set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-cpu/version.h")
include(generate_version_header)
set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-video/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-video/version.h")
include(generate_version_header)

# ============================================================
# Include directories
# ============================================================
include_directories(${CMAKE_BINARY_DIR}/generated)

# external
# ============================================================
add_subdirectory(external)

# ============================================================
# Include source code
# ============================================================
add_subdirectory(src/3rd_party/lz4)
add_subdirectory(src/3rd_party/glad)
add_subdirectory(src/3rd_party/bc_decoder)
add_subdirectory(src/3rd_party/nx_tzdb)
if(NOT ANDROID)
    add_subdirectory(src/3rd_party/discord-rpc)
endif()
add_subdirectory(src/common)
add_subdirectory(src/network)
add_subdirectory(src/yuzu_common)
add_subdirectory(src/yuzu_hid_core)
add_subdirectory(src/yuzu_input_common)
add_subdirectory(src/yuzu_shader_recompiler)
add_subdirectory(src/yuzu_video_core)
add_subdirectory(src/yuzu_audio_core)
add_subdirectory(src/nxemu-core)
add_subdirectory(src/nxemu-cpu)
add_subdirectory(src/nxemu-loader)
add_subdirectory(src/nxemu-os)
add_subdirectory(src/nxemu-video)
if(ANDROID)
    add_subdirectory(src/android/app/src/main/jni)
endif()
if(NOT ANDROID)
    add_subdirectory(src/nxemu)
endif()
