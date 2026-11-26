add_executable(test_hle_zstd_patcher
    hle_zstd_patcher.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader/core/loader/hle_zstd_patcher.cpp")
target_include_directories(test_hle_zstd_patcher PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-loader"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_link_libraries(test_hle_zstd_patcher PRIVATE yuzu_common fmt::fmt)
nxemu_unit_test(test_hle_zstd_patcher LINUX)
