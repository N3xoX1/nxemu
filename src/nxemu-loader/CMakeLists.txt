cmake_minimum_required(VERSION 3.18)
project(nxemu-loader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-loader/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-loader/version.h")
include(generate_version_header)

file(GLOB_RECURSE NXEMU_LOADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(nxemu-loader SHARED ${NXEMU_LOADER_SOURCES})

target_include_directories(nxemu-loader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/boost
    ${CMAKE_SOURCE_DIR}/src/3rd_party/nx_tzdb/include
)

target_compile_definitions(nxemu-loader PRIVATE
    NOMINMAX
    BOOST_ALL_NO_LIB
)

target_link_libraries(nxemu-loader PRIVATE
    common
    yuzu_common
    fmt::fmt
    lz4
    minizip::minizip
)

if(WIN32)
    target_compile_definitions(nxemu-loader PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(ANDROID)
    target_link_libraries(nxemu-loader PRIVATE log)
endif()

set_property(TARGET nxemu-loader PROPERTY POSITION_INDEPENDENT_CODE ON)
