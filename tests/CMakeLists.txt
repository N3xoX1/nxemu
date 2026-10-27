# Unit tests check portable logic and run in CI. Platform tests execute native code on the
# host and need real hardware: run them locally with ctest -L platform.

# Logging and assertion definitions for tests that link single production sources.
add_library(nxemu_test_support OBJECT common/support_stubs.cpp)
target_include_directories(nxemu_test_support PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(nxemu_test_support PRIVATE FMT_HEADER_ONLY)

add_subdirectory(unit)

include(CMakeDependentOption)
cmake_dependent_option(NXEMU_BUILD_PLATFORM_TESTS "Build the native code execution tests" ON
    "APPLE;ARCHITECTURE_arm64" OFF)
if(NXEMU_BUILD_PLATFORM_TESTS)
    add_subdirectory(platform)
endif()
