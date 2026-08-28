cmake_minimum_required(VERSION 3.18)

# nxemu-os depends on yuzu "core", yuzu_audio_core, src/network, hid_core (which itself
# links "core"), cubeb, SDL, etc. Host desktop builds may still be incomplete — default OFF.
# Android needs the OS plugin .so alongside loader/cpu/video, so default ON there.
if(ANDROID)
    set(_NXEMU_OS_BUILD_DEFAULT ON)
else()
    set(_NXEMU_OS_BUILD_DEFAULT OFF)
endif()
option(NXEMU_BUILD_NXEMU_OS "Build nxemu-os shared library" ${_NXEMU_OS_BUILD_DEFAULT})

if(NOT NXEMU_BUILD_NXEMU_OS)
    message(STATUS "nxemu-os: skipped (NXEMU_BUILD_NXEMU_OS=OFF)")
else()

project(nxemu-os LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-os/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-os/version.h")
include(generate_version_header)

file(GLOB_RECURSE NXEMU_OS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
# Each ssl_backend_*.cpp defines Service::SSL::CreateSSLConnectionBackend — compile exactly one.
if(ANDROID)
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_securetransport\\.cpp$")
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_schannel\\.cpp$")
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_none\\.cpp$")
elseif(WIN32)
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_(none|openssl|securetransport)\\.cpp$")
elseif(APPLE)
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_(none|openssl|schannel)\\.cpp$")
else()
    # Linux and other Unix-like hosts: OpenSSL backend
    list(FILTER NXEMU_OS_SOURCES EXCLUDE REGEX ".*/ssl_backend_(none|securetransport|schannel)\\.cpp$")
endif()

add_library(nxemu-os SHARED ${NXEMU_OS_SOURCES})

target_sources(nxemu-os PRIVATE ${CMAKE_SOURCE_DIR}/src/3rd_party/tz/tz.cpp)

target_include_directories(nxemu-os PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/yuzu_video_core
    ${CMAKE_SOURCE_DIR}/src/nxemu-loader
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/boost
    ${CMAKE_SOURCE_DIR}/src/3rd_party/nx_tzdb/include
    ${CMAKE_SOURCE_DIR}/src/3rd_party
    ${CMAKE_SOURCE_DIR}/src/3rd_party/stb
    ${CMAKE_SOURCE_DIR}/external/libusb/libusb
    ${CMAKE_SOURCE_DIR}/external/opus/include
)

target_compile_definitions(nxemu-os PRIVATE
    NOMINMAX
    BOOST_ALL_NO_LIB
)

target_link_libraries(nxemu-os PRIVATE
    common
    hid_core
    input_common
    network
    yuzu_audio_core
    yuzu_common
    fmt::fmt
    lz4
)

if(ANDROID)
    target_link_libraries(nxemu-os PRIVATE log)
endif()

# Use BSD socket headers and TranslateIPv4(in_addr) path (Linux/macOS/Android).
if(UNIX)
    target_compile_definitions(nxemu-os PRIVATE YUZU_UNIX=1)
endif()

# 128-bit atomics used by OS networking / room code — need cmpxchg16b on x86_64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(nxemu-os PRIVATE -mcx16)
    target_link_libraries(nxemu-os PRIVATE atomic)
endif()

set_property(TARGET nxemu-os PROPERTY POSITION_INDEPENDENT_CODE ON)

nxemu_set_module_output(nxemu-os operating_system)

endif()
