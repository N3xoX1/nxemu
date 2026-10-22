cmake_minimum_required(VERSION 3.20)
project(nxemu_query_lifetime_test LANGUAGES CXX)
find_package(Vulkan REQUIRED)
add_executable(query-reset-lifetime query_reset_lifetime.cpp)
target_compile_features(query-reset-lifetime PRIVATE cxx_std_20)
target_link_libraries(query-reset-lifetime PRIVATE Vulkan::Vulkan)

add_executable(query-bank-operations query_bank_operations_test.cpp)
target_compile_features(query-bank-operations PRIVATE cxx_std_20)
target_include_directories(query-bank-operations PRIVATE ../../src/yuzu_video_core/query_cache)

include(CTest)
add_test(NAME query-reset-lifetime COMMAND query-reset-lifetime)
add_test(NAME query-bank-operations COMMAND query-bank-operations)
