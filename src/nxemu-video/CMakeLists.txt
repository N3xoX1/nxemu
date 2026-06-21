cmake_minimum_required(VERSION 3.18)
project(nxemu-video LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-video/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-video/version.h")
include(generate_version_header)

set(NXEMU_VIDEO_SOURCES
    nxemu-video.cpp
    render_window.cpp
    video_manager.cpp
    video_manager.h
    video_enum_strings.cpp
    video_enum_strings.h
    video_settings.cpp
    video_settings.h
    video_settings_identifiers.h
    version.h
    render_window.h
)

add_library(nxemu-video SHARED ${NXEMU_VIDEO_SOURCES})

target_include_directories(nxemu-video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/yuzu_video_core
    ${CMAKE_SOURCE_DIR}/external/boost
)

target_link_libraries(nxemu-video PRIVATE yuzu_common)

if(TARGET video_core)
    target_link_libraries(nxemu-video PRIVATE video_core)
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_link_libraries(nxemu-video PRIVATE adrenotools)
endif()

if(ANDROID)
    target_link_libraries(nxemu-video PRIVATE log android)
    target_link_options(nxemu-video PRIVATE "LINKER:-Bsymbolic")
endif()
