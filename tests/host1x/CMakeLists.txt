cmake_minimum_required(VERSION 3.22)
project(nxemu_host1x_tests LANGUAGES CXX)

enable_testing()
get_filename_component(NXEMU_ROOT "../.." ABSOLUTE)

# Compile the production parser against recording devices. No GPU, FFmpeg,
# firmware or game files are needed to verify command routing.
set(STUB_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/include")
foreach(header IN ITEMS
    engines/maxwell_3d.h
    host1x/control.h
    host1x/host1x.h
    host1x/nvdec.h
    host1x/nvdec_common.h
    host1x/sync_manager.h
    host1x/vic.h
    memory_manager.h)
    file(WRITE "${STUB_INCLUDE}/yuzu_video_core/${header}"
        "#include \"probe-devices.h\"\n")
endforeach()

add_executable(cdma_registers cdma_registers.cpp
    "${NXEMU_ROOT}/src/yuzu_video_core/cdma_pusher.cpp")
target_compile_features(cdma_registers PRIVATE cxx_std_20)
target_compile_definitions(cdma_registers PRIVATE NOMINMAX)
target_include_directories(cdma_registers PRIVATE
    "${STUB_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}" "${NXEMU_ROOT}/src")
add_test(NAME cdma_registers COMMAND cdma_registers)
