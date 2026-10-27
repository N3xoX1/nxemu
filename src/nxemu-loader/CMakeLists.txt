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

add_library(nxemu_loader_objects OBJECT ${NXEMU_LOADER_SOURCES})
add_library(nxemu-loader SHARED)

target_include_directories(nxemu_loader_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/boost
)

target_compile_definitions(nxemu_loader_objects PRIVATE
    NOMINMAX
    BOOST_ALL_NO_LIB
)

target_link_libraries(nxemu_loader_objects PUBLIC
    common
    yuzu_common
    fmt::fmt
    lz4
    minizip::minizip
    nx_tzdb
)
target_link_libraries(nxemu-loader PRIVATE nxemu_loader_objects)

if(WIN32)
    target_compile_definitions(nxemu_loader_objects PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(ANDROID)
    target_link_libraries(nxemu-loader PRIVATE log)
endif()

set_property(TARGET nxemu-loader PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET nxemu_loader_objects PROPERTY POSITION_INDEPENDENT_CODE ON)

nxemu_set_module_output(nxemu-loader loader)
