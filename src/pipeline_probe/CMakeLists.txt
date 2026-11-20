# Diagnostic tool: replays a pipeline captured by the pipeline crash trace on this GPU.
add_executable(pipeline_probe main.cpp)
target_compile_features(pipeline_probe PRIVATE cxx_std_20)
target_include_directories(pipeline_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pipeline_probe PRIVATE Vulkan::Headers)
nxemu_set_app_output(pipeline_probe)
