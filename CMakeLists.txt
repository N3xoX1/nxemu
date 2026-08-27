cmake_minimum_required(VERSION 3.22)

project(nxemu)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================
# Architecture detection
# ============================================================
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(ARCHITECTURE_arm64 TRUE)
    set(ARCHITECTURE "arm64")
    set(NXEMU_PLATFORM_DIR "arm64")
    add_compile_definitions(ARCHITECTURE_arm64=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_definitions(ARCHITECTURE_x86_64=1)
    set(ARCHITECTURE_x86_64 TRUE)
    set(ARCHITECTURE "x86_64")
    set(NXEMU_PLATFORM_DIR "x64")
else()
    set(NXEMU_PLATFORM_DIR "${CMAKE_SYSTEM_PROCESSOR}")
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
if(NOT ANDROID)
    add_subdirectory(src/nxemu)
endif()
