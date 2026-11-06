# Uniform reuse and reverse descriptor index regression coverage.
add_executable(test_texture_descriptor_pr5 pr5_regression.cpp)
target_include_directories(test_texture_descriptor_pr5 PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external/boost")
target_compile_features(test_texture_descriptor_pr5 PRIVATE cxx_std_20)
nxemu_unit_test(test_texture_descriptor_pr5 LINUX)
