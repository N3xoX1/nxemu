cmake_minimum_required(VERSION 3.22)

# Let CMake select /Zi or /Z7 for MSVC, including cached compilations.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
endif()

# Match the minimum supported by the bundled MoltenVK runtime, instead of
# silently targeting the macOS version of the developer's machine.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND
   (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND
   NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(nxemu)

# Make opt-in tests in subdirectories discoverable from the build root.
enable_testing()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.0")
        message(FATAL_ERROR "The bundled MoltenVK runtime requires macOS 12.0 or newer")
    endif()
    enable_language(OBJC OBJCXX)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

find_package(Threads REQUIRED)

if(NOT ANDROID)
    option(ENABLE_SDL "Use SDL for controller input and audio output" ON)
    option(ENABLE_LIBUSB "Enable direct GameCube USB adapter input" ON)
endif()
if(APPLE)
    # Use Boost.Container for overlapping ICL interval lookups on libc++.
    add_compile_definitions(ICL_USE_BOOST_MOVE_IMPLEMENTATION)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(ANDROID)
    # NDK r26 defaults to 4 KiB ELF alignment. Also allow Android devices
    # using 16 KiB host pages; the guest's 4 KiB pages remain unchanged.
    add_link_options("LINKER:-z,max-page-size=16384" "LINKER:-z,common-page-size=16384")
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
include(nxemu_msvc)
include(nxemu_output_directories)
include(nxemu_ccache)
include(nxemu_lto)

set(NXEMU_COMPILE_JOBS "" CACHE STRING "Optional maximum concurrent Ninja compile jobs")
if(NXEMU_COMPILE_JOBS)
    if(NOT NXEMU_COMPILE_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "NXEMU_COMPILE_JOBS must be a positive integer")
    endif()
    if(CMAKE_GENERATOR MATCHES "Ninja")
        set_property(GLOBAL APPEND PROPERTY JOB_POOLS nxemu_compile=${NXEMU_COMPILE_JOBS})
        set(CMAKE_JOB_POOL_COMPILE nxemu_compile)
    endif()
endif()

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
if(WIN32)
    add_compile_definitions(NOMINMAX BOOST_ALL_NO_LIB)
endif()
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
    # Visual Studio starts the emulator, not ALL_BUILD.
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT nxemu)
endif()

option(NXEMU_BUILD_TESTS "Build the tests in tests/" OFF)
if(NXEMU_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# These targets correspond to projects that import platform.props in nxemu.sln.
# Do not propagate /GS- to additional dependency targets, examples or tests.
if(MSVC)
    foreach(_target nxemu nxemu_ui nxemu_core nxemu-cpu nxemu-loader nxemu-os nxemu-video
            common network yuzu_common hid_core input_common shader_recompiler video_core
            yuzu_audio_core ui_packer bc_decoder discord-rpc glad lz4 stb_dxt
            fmt nxemu_libusb SDL3-shared rmlui_core freetype enet mcl opus sirit
            libzstd_static zlibstatic minizip Zydis Zycore cubeb speex)
        nxemu_apply_legacy_stack_settings(${_target})
    endforeach()
    unset(_target)
endif()
