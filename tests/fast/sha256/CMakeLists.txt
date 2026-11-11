add_executable(test_common_sha256 sha256.cpp "${NXEMU_FAST_TEST_SOURCE_DIR}/common/sha256.cpp")
target_include_directories(test_common_sha256 PRIVATE "${NXEMU_FAST_TEST_SOURCE_DIR}")
nxemu_fast_test(test_common_sha256)
