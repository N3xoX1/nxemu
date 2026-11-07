find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(extracted "${CMAKE_CURRENT_BINARY_DIR}/extracted")
add_custom_command(OUTPUT "${extracted}/visited_blocks.inc" "${extracted}/ordered.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py"
        "${CMAKE_SOURCE_DIR}" "${extracted}"
    DEPENDS extract.py
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/frontend/maxwell/control_flow.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/frontend/maxwell/structured_control_flow.cpp"
    VERBATIM)
add_executable(test_lichii_graph_search search.cpp)
add_executable(test_lichii_control_flow control_flow.cpp
    "${extracted}/visited_blocks.inc" "${extracted}/ordered.inc")
target_include_directories(test_lichii_control_flow PRIVATE "${extracted}")
foreach(target test_lichii_graph_search test_lichii_control_flow)
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE shader_recompiler nxemu_test_support)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    nxemu_unit_test(${target} LINUX)
endforeach()
# Explicitly built microbenchmark, using Release flags without sanitizer overhead.
add_executable(bench_lichii_graph_search EXCLUDE_FROM_ALL search.cpp)
target_include_directories(bench_lichii_graph_search PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(bench_lichii_graph_search PRIVATE shader_recompiler nxemu_test_support)

add_executable(test_lichii_descriptor_memory descriptor_memory.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/memory_manager.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/gpu_device_memory_manager.cpp")
target_include_directories(test_lichii_descriptor_memory PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include")
add_dependencies(test_lichii_descriptor_memory video_core yuzu_common common)
target_link_libraries(test_lichii_descriptor_memory PRIVATE nxemu_test_support
    $<TARGET_FILE:video_core> $<TARGET_FILE:yuzu_common> $<TARGET_FILE:common>
    fmt bc_decoder stb_dxt)
if(NOT MSVC)
    target_compile_options(test_lichii_descriptor_memory PRIVATE
        -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_lichii_descriptor_memory PRIVATE -fsanitize=address,undefined)
endif()
nxemu_unit_test(test_lichii_descriptor_memory LINUX)
