set(_commit_inc "${CMAKE_CURRENT_BINARY_DIR}/commit.inc")
add_custom_command(OUTPUT "${_commit_inc}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py"
        "${CMAKE_SOURCE_DIR}" "${_commit_inc}"
    DEPENDS extract.py "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/dynarmic/backend/x64/block_of_code.cpp"
    VERBATIM)
foreach(mode rw rwx)
    add_executable(test_jit_commit_${mode} commit.cpp "${_commit_inc}")
    target_include_directories(test_jit_commit_${mode} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    if(mode STREQUAL "rw")
        target_compile_definitions(test_jit_commit_${mode} PRIVATE DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    endif()
    nxemu_unit_test(test_jit_commit_${mode} LINUX)
endforeach()
add_executable(test_pipeline_module_gpu EXCLUDE_FROM_ALL gpu.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_enum_strings.cpp")
target_include_directories(test_pipeline_module_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
    "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/boost")
target_link_libraries(test_pipeline_module_gpu PRIVATE nxemu_test_support video_core
    Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS})
