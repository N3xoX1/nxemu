# Exercise the production codec, cache and import/export implementation without a GPU
# or emulator session. Executables provide their own minimal renderer/logging stubs.
add_library(nxemu_shader_profile_test_sources STATIC
    "${CMAKE_SOURCE_DIR}/src/yuzu_common/cityhash.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/pipeline_cache_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/shader_profile.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/shader_profile_codec.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/shader_profile_export.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/shader_profile_discovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/shader_profile_compact.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/renderer_vulkan/fixed_pipeline_state.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/surface.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/sha256.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/dynamic_library.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/json.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/std_string.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/platform.cpp"
)
target_include_directories(nxemu_shader_profile_test_sources PUBLIC
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include"
    "${CMAKE_SOURCE_DIR}/external/vulkan-headers/include")
target_compile_features(nxemu_shader_profile_test_sources PUBLIC cxx_std_20)
target_compile_definitions(nxemu_shader_profile_test_sources PUBLIC
    FMT_HEADER_ONLY NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nxemu_shader_profile_test_sources PUBLIC Threads::Threads
    PRIVATE zstd::zstd ${CMAKE_DL_LIBS})
if(MSVC)
    target_compile_options(nxemu_shader_profile_test_sources PUBLIC /utf-8 /EHsc)
endif()

add_custom_target(test_shader_profile)
function(nxemu_shader_profile_test name)
    set(target "test_shader_profile_${name}")
    add_executable(${target} "${name}.cpp")
    target_link_libraries(${target} PRIVATE nxemu_shader_profile_test_sources)
    add_test(NAME ${target} COMMAND ${target} ${ARGN})
    set_tests_properties(${target} PROPERTIES LABELS "unit;shader_profile" TIMEOUT 120)
    add_dependencies(test_shader_profile ${target})
endfunction()

nxemu_shader_profile_test(cache_store "${CMAKE_CURRENT_BINARY_DIR}/regression.nxc")
nxemu_shader_profile_test(candidate_warmup_budget)
nxemu_shader_profile_test(cross_version_reuse)
nxemu_shader_profile_test(driver_cache_probe)
nxemu_shader_profile_test(guest_pipeline_state)
nxemu_shader_profile_test(profile_codec_regression)
nxemu_shader_profile_test(profile_hash)
nxemu_shader_profile_test(profile_merge)
nxemu_shader_profile_test(profile_metrics)
nxemu_shader_profile_test(profile_search)
nxemu_shader_profile_test(progressive_warmup_policy)
nxemu_shader_profile_test(quarantine_runtime "${CMAKE_CURRENT_BINARY_DIR}/runtime-report.json")
nxemu_shader_profile_test(recipe_journal "${CMAKE_CURRENT_BINARY_DIR}/regression.nxr")
nxemu_shader_profile_test(recorded_sources "${CMAKE_CURRENT_BINARY_DIR}/recorded-sources-fixture")
nxemu_shader_profile_test(recorder_input_cache)
nxemu_shader_profile_test(recorder_replay "${CMAKE_CURRENT_BINARY_DIR}/recorder-replay-")
nxemu_shader_profile_test(startup_variants)
nxemu_shader_profile_test(synthetic_robustness)
