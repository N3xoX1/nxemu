cmake_minimum_required(VERSION 3.22)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(nxemu_fast_tests LANGUAGES CXX)
    enable_testing()
endif()

set(NXEMU_FAST_TEST_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
add_custom_target(nxemu_fast_tests)

function(nxemu_fast_test target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    add_dependencies(nxemu_fast_tests ${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit;fast" TIMEOUT 120)
endfunction()

file(GLOB fast_suites RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(suite IN LISTS fast_suites)
    get_filename_component(suite_directory "${suite}" DIRECTORY)
    add_subdirectory("${suite_directory}")
endforeach()
