function(nxemu_unit_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 120)
endfunction()

# Host thread state and Host1x syncpoint cancellation, under ASan and UBSan.
add_executable(test_session_lifecycle
    session_lifecycle/test_session_lifecycle.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/syncpoint_manager.cpp")
target_include_directories(test_session_lifecycle PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_session_lifecycle PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(test_session_lifecycle PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_session_lifecycle PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_session_lifecycle)

# SPIR-V layout of constant buffers. Link the archives themselves: their dependencies would
# bring a second definition of the logging functions.
add_executable(test_spirv_constant_buffers spirv_constant_buffers/layout.cpp)
target_include_directories(test_spirv_constant_buffers PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/include"
    "${CMAKE_SOURCE_DIR}/external/sirit/externals/SPIRV-Headers/include")
add_dependencies(test_spirv_constant_buffers shader_recompiler sirit fmt common)
target_link_libraries(test_spirv_constant_buffers PRIVATE nxemu_test_support
    $<TARGET_FILE:shader_recompiler> $<TARGET_FILE:sirit> $<TARGET_FILE:fmt> $<TARGET_FILE:common>)
nxemu_unit_test(test_spirv_constant_buffers)

# Application control metadata (NACP) parsing.
add_executable(test_loader_metadata loader_metadata/nacp.cpp)
target_include_directories(test_loader_metadata PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_loader_metadata PRIVATE nxemu-loader)
nxemu_unit_test(test_loader_metadata)

add_subdirectory(shader_profile)
