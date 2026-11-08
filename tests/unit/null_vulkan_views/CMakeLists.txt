add_executable(test_null_vulkan_views_gpu EXCLUDE_FROM_ALL gpu.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-video/video_enum_strings.cpp")
target_include_directories(test_null_vulkan_views_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/yuzu_video_core"
    "${CMAKE_SOURCE_DIR}/src/nxemu-video" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/tests/unit/fast_copy")
target_link_libraries(test_null_vulkan_views_gpu PRIVATE nxemu_test_support video_core
    Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator ${CMAKE_DL_LIBS})
add_executable(test_null_vulkan_view_descriptions descriptions.cpp)
target_include_directories(test_null_vulkan_view_descriptions PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_null_vulkan_view_descriptions PRIVATE Vulkan::Headers fmt boost_headers)
nxemu_unit_test(test_null_vulkan_view_descriptions LINUX)
