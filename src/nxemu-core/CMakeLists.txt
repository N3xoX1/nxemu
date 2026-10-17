add_library(nxemu_core STATIC
    modules/cpu_module.cpp
    modules/loader_module.cpp
    modules/module_base.cpp
    modules/module_notification.cpp
    modules/module_settings.cpp
    modules/operating_system_module.cpp
    modules/system_modules.cpp
    modules/video_module.cpp
    settings/game_settings.cpp
    settings/game_settings.h
    settings/core_settings.cpp
    settings/settings.cpp
    app_init.cpp
    logging.cpp
    notification.cpp
)

target_include_directories(nxemu_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nxemu_core PRIVATE common yuzu_common)
option(NXEMU_BUILD_GAME_SETTINGS_TESTS "Build game override integration tests" OFF)
if(NXEMU_BUILD_GAME_SETTINGS_TESTS)
    add_executable(nxemu_game_settings_tests
        settings/game_settings_tests.cpp
        ../nxemu-video/video_settings.cpp
        ../nxemu-video/video_enum_strings.cpp
        ../nxemu-cpu/cpu_settings.cpp
        ../nxemu-cpu/cpu_enum_strings.cpp)
    target_link_libraries(nxemu_game_settings_tests PRIVATE nxemu_core common yuzu_common)
    enable_testing()
    add_test(NAME nxemu_game_settings COMMAND nxemu_game_settings_tests
        "${CMAKE_SOURCE_DIR}/src/nxemu/ui/assets/game_overrides.json")
endif()
