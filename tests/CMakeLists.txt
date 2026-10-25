# Logging and assertion definitions for tests that link single production sources.
add_library(nxemu_test_support OBJECT common/support_stubs.cpp)
target_include_directories(nxemu_test_support PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(nxemu_test_support PRIVATE FMT_HEADER_ONLY)

add_subdirectory(unit)
