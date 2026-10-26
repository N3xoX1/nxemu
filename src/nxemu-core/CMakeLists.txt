add_library(nxemu_core STATIC
    modules/cpu_module.cpp
    modules/loader_module.cpp
    modules/module_base.cpp
    modules/module_notification.cpp
    modules/module_settings.cpp
    modules/operating_system_module.cpp
    modules/system_modules.cpp
    modules/video_module.cpp
    settings/core_settings.cpp
    settings/settings.cpp
    app_init.cpp
    logging.cpp
    notification.cpp
    play_time.cpp
)

target_include_directories(nxemu_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nxemu_core PRIVATE common yuzu_common)