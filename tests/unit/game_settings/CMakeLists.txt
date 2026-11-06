add_executable(test_game_settings game_settings.cpp)
target_include_directories(test_game_settings PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_game_settings PRIVATE nxemu_core common yuzu_common)
nxemu_unit_test(test_game_settings LINUX)

add_executable(test_module_setting_table
    module_setting_table.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/cpu_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/cpu_enum_strings.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/json.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/json_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/std_string.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/platform.cpp")
target_include_directories(test_module_setting_table PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_module_setting_table PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_module_setting_table PRIVATE nxemu_test_support)
nxemu_unit_test(test_module_setting_table LINUX)
