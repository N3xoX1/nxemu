option(NXEMU_MACOS_BUNDLE "Build NxEmu as a macOS application bundle" ON)

add_executable(nxemu
    main.cpp
    startup_checks.cpp
    startup_checks.h
    vulkan_probe.cpp
    vulkan_probe.h
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
    user_interface/file_dialogs.cpp
    user_interface/file_dialogs.h
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
    user_interface/settings/profile_editor_dialog.cpp
    user_interface/settings/profile_editor_dialog.h
    user_interface/settings/profile_firmware_avatar_dialog.cpp
    user_interface/settings/profile_firmware_avatar_dialog.h
    user_interface/settings/profile_image_selector_dialog.cpp
    user_interface/settings/profile_image_selector_dialog.h
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
    user_interface/settings/system_config_input.cpp
    user_interface/settings/system_config_input.h
    user_interface/settings/system_config_logging.cpp
    user_interface/settings/system_config_logging.h
    user_interface/settings/system_config_profiles.cpp
    user_interface/settings/system_config_profiles.h
    user_interface/settings/system_config_system.cpp
    user_interface/settings/system_config_system.h
    user_interface/settings/system_config_system_tab.cpp
    user_interface/settings/system_config_system_tab.h
    applets/profile_select.cpp
    applets/profile_select.h
    applets/web_browser.cpp
    applets/web_browser.h
)

if(APPLE)
    target_sources(nxemu PRIVATE
        user_interface/file_dialogs_macos.mm
        user_interface/render_window_macos.mm
    )
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_link_libraries(nxemu PRIVATE
        ${APPKIT_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
    )
endif()

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

if(UNIX)
    target_compile_definitions(nxemu PRIVATE NXEMU_UNIX=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS dbus-1 REQUIRED IMPORTED_TARGET)
    target_link_libraries(nxemu PRIVATE PkgConfig::DBUS)
endif()

target_link_libraries(nxemu PRIVATE
    sciterui
    common
    yuzu_common
    nxemu_core
    discord-rpc
    fmt::fmt
    Vulkan::Headers
    Vulkan::UtilityHeaders
    ${CMAKE_DL_LIBS}
)
nxemu_set_app_output(nxemu)
if(NOT APPLE OR NOT NXEMU_MACOS_BUNDLE)
    include(nxemu_setup_environment)
    nxemu_setup_environment(nxemu)
endif()

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

if(APPLE AND NXEMU_MACOS_BUNDLE)
    set(_nxemu_icon "${CMAKE_CURRENT_SOURCE_DIR}/macos/nxemu.icns")
    set_source_files_properties("${_nxemu_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(nxemu PRIVATE "${_nxemu_icon}")
    set_target_properties(nxemu PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "nxemu.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.nxemu"
        MACOSX_BUNDLE_BUNDLE_NAME "NxEmu"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_BUILD}"
        OUTPUT_NAME "NxEmu"
    )
    if(NOT NXEMU_MOLTENVK_RUNTIME OR NOT EXISTS "${_nxemu_sciter_lib}")
        message(FATAL_ERROR "The macOS bundle requires MoltenVK and Sciter")
    endif()
    if(TARGET nxemu_moltenvk)
        add_dependencies(nxemu nxemu_moltenvk)
    endif()
    add_dependencies(nxemu nxemu-loader nxemu-cpu nxemu-video nxemu-os)
    find_program(NXEMU_CODESIGN codesign REQUIRED)
    set(_nxemu_bundle_script "${CMAKE_SOURCE_DIR}/src/cmake/modules/nxemu_macos_bundle.cmake")
    set(_nxemu_bundle_command ${CMAKE_COMMAND}
        "-DBUNDLE=$<TARGET_BUNDLE_DIR:nxemu>"
        "-DLOADER=$<TARGET_FILE:nxemu-loader>"
        "-DCPU=$<TARGET_FILE:nxemu-cpu>"
        "-DVIDEO=$<TARGET_FILE:nxemu-video>"
        "-DOS=$<TARGET_FILE:nxemu-os>"
        "-DMOLTENVK=${NXEMU_MOLTENVK_RUNTIME}"
        "-DSCITER=${_nxemu_sciter_lib}"
        "-DLANG=${CMAKE_SOURCE_DIR}/lang"
        "-DCODESIGN=${NXEMU_CODESIGN}"
        "-DINFO_PLIST=${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        "-DICON=${_nxemu_icon}"
        "-DEXECUTABLE_NAME=$<TARGET_FILE_NAME:nxemu>"
        "-DSHORT_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}"
        "-DBUILD_VERSION=${VERSION_BUILD}"
        "-DMINIMUM_MACOS=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        -P "${_nxemu_bundle_script}")
    # Module-only rebuilds must refresh the app even when its own code did not change.
    # LINK_DEPENDS also covers an explicit --target nxemu build.
    set_property(TARGET nxemu APPEND PROPERTY LINK_DEPENDS
        "$<TARGET_FILE:nxemu-loader>" "$<TARGET_FILE:nxemu-cpu>"
        "$<TARGET_FILE:nxemu-video>" "$<TARGET_FILE:nxemu-os>"
        "${NXEMU_MOLTENVK_RUNTIME}" "${_nxemu_sciter_lib}" "${_nxemu_bundle_script}"
        "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in" "${_nxemu_icon}")
    add_custom_command(TARGET nxemu POST_BUILD
        COMMAND ${_nxemu_bundle_command} VERBATIM
        COMMENT "Package and sign NxEmu.app with this build's modules")
    add_custom_target(nxemu_bundle ALL
        COMMAND ${_nxemu_bundle_command}
        DEPENDS nxemu
        VERBATIM
        COMMENT "Refresh NxEmu.app runtime and language resources")
else()
    if(EXISTS "${_nxemu_sciter_lib}")
        add_custom_command(TARGET nxemu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_nxemu_sciter_lib}" "$<TARGET_FILE_DIR:nxemu>/${_nxemu_sciter_dest}"
            VERBATIM)
    endif()
    if(APPLE)
        if(NOT NXEMU_MOLTENVK_RUNTIME)
            message(FATAL_ERROR "MoltenVK runtime was not configured")
        endif()
        if(TARGET nxemu_moltenvk)
            add_dependencies(nxemu nxemu_moltenvk)
        endif()
        add_dependencies(nxemu nxemu-loader nxemu-cpu nxemu-video nxemu-os)
        set_property(TARGET nxemu APPEND PROPERTY LINK_DEPENDS
            "$<TARGET_FILE:nxemu-loader>" "$<TARGET_FILE:nxemu-cpu>"
            "$<TARGET_FILE:nxemu-video>" "$<TARGET_FILE:nxemu-os>")
        add_custom_command(TARGET nxemu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NXEMU_MOLTENVK_RUNTIME}" "$<TARGET_FILE_DIR:nxemu>/libMoltenVK.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lang" "$<TARGET_FILE_DIR:nxemu>/lang"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:nxemu>/modules/loader"
                "$<TARGET_FILE_DIR:nxemu>/modules/cpu"
                "$<TARGET_FILE_DIR:nxemu>/modules/video"
                "$<TARGET_FILE_DIR:nxemu>/modules/operating_system"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nxemu-loader>" "$<TARGET_FILE_DIR:nxemu>/modules/loader/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nxemu-cpu>" "$<TARGET_FILE_DIR:nxemu>/modules/cpu/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nxemu-video>" "$<TARGET_FILE_DIR:nxemu>/modules/video/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nxemu-os>" "$<TARGET_FILE_DIR:nxemu>/modules/operating_system/"
            VERBATIM)
    endif()
endif()
