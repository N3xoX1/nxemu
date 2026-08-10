# Audio engine (matches src/yuzu_audio_core/yuzu_audio_core.vcxproj).
cmake_minimum_required(VERSION 3.18)

if(NOT TARGET cubeb)
    set(USE_SANITIZERS OFF CACHE BOOL "cubeb: skip sanitizers-cmake" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "cubeb: skip gtest" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "cubeb: skip tools" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/cubeb" "${CMAKE_BINARY_DIR}/external/cubeb")
endif()

file(GLOB_RECURSE YUZU_AUDIO_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

list(FILTER YUZU_AUDIO_CORE_SOURCES EXCLUDE REGEX ".*/sdl2_sink\\.cpp$")
list(FILTER YUZU_AUDIO_CORE_SOURCES EXCLUDE REGEX ".*/oboe_sink\\.cpp$")

add_library(yuzu_audio_core STATIC ${YUZU_AUDIO_CORE_SOURCES})

target_include_directories(yuzu_audio_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/nxemu-os
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/boost
    ${CMAKE_SOURCE_DIR}/external/fmt/include
    ${CMAKE_SOURCE_DIR}/external/opus/include
    ${CMAKE_SOURCE_DIR}/src/3rd_party/microprofile
)

target_compile_definitions(yuzu_audio_core PUBLIC
    HAVE_CUBEB=1
    NOMINMAX
)

target_link_libraries(yuzu_audio_core PUBLIC
    common
    yuzu_common
    fmt::fmt
    lz4
    Opus::opus
    PRIVATE
    cubeb
)

if(ANDROID)
    target_link_libraries(yuzu_audio_core PRIVATE log)
endif()

set_property(TARGET yuzu_audio_core PROPERTY POSITION_INDEPENDENT_CODE ON)
