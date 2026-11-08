find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(extracted "${CMAKE_CURRENT_BINARY_DIR}/extracted")
add_custom_command(OUTPUT "${extracted}/demote.inc" "${extracted}/find_buffer.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py"
        "${CMAKE_SOURCE_DIR}" "${extracted}"
    DEPENDS extract.py
        "${CMAKE_SOURCE_DIR}/src/yuzu_shader_recompiler/frontend/maxwell/structured_control_flow.cpp"
        "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/buffer_cache/buffer_cache.h"
    VERBATIM)
add_executable(test_shader_demote demote.cpp "${extracted}/demote.inc")
add_executable(test_buffer_cache_bounds buffer_bounds.cpp "${extracted}/find_buffer.inc")
target_include_directories(test_shader_demote PRIVATE "${extracted}" "${CMAKE_SOURCE_DIR}/src")
target_include_directories(test_buffer_cache_bounds PRIVATE "${extracted}")
target_link_libraries(test_shader_demote PRIVATE shader_recompiler nxemu_test_support)
add_executable(test_shader_phi phi.cpp)
target_include_directories(test_shader_phi PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_shader_phi PRIVATE shader_recompiler nxemu_test_support)
foreach(target test_shader_demote test_buffer_cache_bounds test_shader_phi)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    nxemu_unit_test(${target} LINUX)
endforeach()
