# The in-tree SHA-256, through the CPU's SHA instructions where the host has them.
add_executable(test_sha256 sha256_vectors.cpp "${NXEMU_FAST_TEST_SOURCE_DIR}/common/sha256.cpp")
target_include_directories(test_sha256 PRIVATE "${NXEMU_FAST_TEST_SOURCE_DIR}")
nxemu_fast_test(test_sha256)
