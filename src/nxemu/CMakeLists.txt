add_executable(nxemu
    main.cpp
    startup_checks.cpp
    startup_checks.h
    settings/ui_settings.cpp
    settings/ui_settings.h
    settings/ui_identifiers.h
    user_interface/about_dialog.cpp
    user_interface/about_dialog.h
    user_interface/discord_presence.cpp
    user_interface/discord_presence.h
    user_interface/game_list_worker.cpp
    user_interface/game_list_worker.h
    user_interface/hotkey_map.cpp
    user_interface/hotkey_map.h
    user_interface/html_utils.cpp
    user_interface/html_utils.h
    user_interface/key_mappings.cpp
    user_interface/key_mappings.h
    user_interface/notification.cpp
    user_interface/notification.h
    user_interface/app_events.h
    user_interface/sciter_main_window.cpp
    user_interface/sciter_main_window.h
    user_interface/widgets/rom_browser.cpp
    user_interface/widgets/rom_browser.h
    user_interface/settings/config_setting.cpp
    user_interface/settings/config_setting.h
    user_interface/settings/game_config.cpp
    user_interface/settings/game_config.h
    user_interface/settings/game_config_addons.cpp
    user_interface/settings/game_config_addons.h
    user_interface/settings/input_config.cpp
    user_interface/settings/input_config.h
    user_interface/settings/input_config_player.cpp
    user_interface/settings/input_config_player.h
    user_interface/settings/system_config.cpp
    user_interface/settings/system_config.h
    user_interface/settings/system_config_audio.cpp
    user_interface/settings/system_config_audio.h
    user_interface/settings/system_config_game_browser.cpp
    user_interface/settings/system_config_game_browser.h
    user_interface/settings/system_config_general.cpp
    user_interface/settings/system_config_general.h
    user_interface/settings/system_config_graphics.cpp
    user_interface/settings/system_config_graphics.h
    user_interface/settings/system_config_hotkeys.cpp
    user_interface/settings/system_config_hotkeys.h
    user_interface/settings/system_config_logging.cpp
    user_interface/settings/system_config_logging.h
    user_interface/settings/system_config_system.cpp
    user_interface/settings/system_config_system.h
    applets/profile_select.cpp
    applets/profile_select.h
    applets/web_browser.cpp
    applets/web_browser.h
)

target_include_directories(nxemu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/nxemu
    ${CMAKE_SOURCE_DIR}/src/nxemu-core
    ${CMAKE_SOURCE_DIR}/src/nxemu-module-spec
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/yuzu_common
    ${CMAKE_SOURCE_DIR}/external/boost
    ${CMAKE_SOURCE_DIR}/external/sciterui/include
)

target_compile_definitions(nxemu PRIVATE)

target_link_libraries(nxemu PRIVATE
    sciterui
    common
    yuzu_common
    nxemu_core
    fmt::fmt
    Vulkan::Headers
    ${CMAKE_DL_LIBS}
)
nxemu_set_app_output(nxemu)

# Runtime needs the Sciter engine beside the executable
if(WIN32)
    set(_nxemu_sciter_lib "${CMAKE_SOURCE_DIR}/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/windows.d2d/x64/sciter.dll")
    set(_nxemu_sciter_dest "sciter.dll")
elseif(APPLE)
    set(_nxemu_sciter_lib "${CMAKE_SOURCE_DIR}/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/macosx/libsciter.dylib")
    set(_nxemu_sciter_dest "libsciter.dylib")
elseif(ARCHITECTURE_arm64)
    set(_nxemu_sciter_lib "${CMAKE_SOURCE_DIR}/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/linux/arm64/libsciter.so")
    set(_nxemu_sciter_dest "libsciter.so")
else()
    set(_nxemu_sciter_lib "${CMAKE_SOURCE_DIR}/external/sciterui/src/3rd_party/sciter-js-sdk-main/bin/linux/x64/libsciter.so")
    set(_nxemu_sciter_dest "libsciter.so")
endif()

if(EXISTS "${_nxemu_sciter_lib}")
    add_custom_command(TARGET nxemu POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_nxemu_sciter_lib}" "$<TARGET_FILE_DIR:nxemu>/${_nxemu_sciter_dest}"
        COMMENT "Copy Sciter engine next to nxemu"
    )
endif()
