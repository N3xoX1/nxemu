set(_nxemu_rmlui_backends "${CMAKE_SOURCE_DIR}/external/rmlui/Backends")

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED CONFIG)
endif()

# The RmlUi frontend. Everything here is shared by all platforms, apart from
# the few window conventions macOS needs set natively.
add_library(nxemu_ui STATIC
    ../settings/ui_settings.cpp
    ../settings/ui_settings.h
    ../settings/ui_identifiers.h
    ui_application.cpp
    ui_application.h
    emulation/emulator_session.cpp
    emulation/emulator_session.h
    host/discord_presence.cpp
    host/discord_presence.h
    host/process_usage.cpp
    host/process_usage.h
    host/sdl_input.cpp
    host/sdl_input.h
    host/shell.cpp
    host/shell.h
    host/system_language.cpp
    host/system_language.h
    host/system_fonts.cpp
    host/system_fonts.h
    host/ui_notification.cpp
    host/ui_notification.h
    host/ui_pack.cpp
    host/ui_pack.h
    host/ui_renderer.cpp
    host/ui_renderer.h
    host/ui_renderer_effects.cpp
    host/ui_renderer_internal.h
    host/ui_theme.cpp
    host/ui_theme.h
    host/ui_window.cpp
    host/ui_window.h
    host/vulkan/vma.cpp
    host/vulkan/vma.h
    host/vulkan/vulkan_api.cpp
    host/vulkan/vulkan_api.h
    i18n/translation.cpp
    i18n/translation.h
    input/input_params.cpp
    input/input_params.h
    input/input_text.cpp
    input/input_text.h
    library/firmware_avatars.cpp
    library/firmware_avatars.h
    library/firmware_installer.cpp
    library/firmware_installer.h
    library/interface_language.cpp
    library/interface_language.h
    library/game_cache.cpp
    library/game_cache.h
    library/game_details.cpp
    library/game_details.h
    library/game_library.cpp
    library/game_library.h
    library/play_time.cpp
    library/play_time.h
    library/rom_properties.cpp
    library/rom_properties.h
    platform/file_dialogs.cpp
    platform/file_dialogs.h
    platform/native_window.h
    platform/system_theme.cpp
    platform/system_theme.h
    views/main_window.cpp
    views/main_window.h
    views/menu_bar.cpp
    views/menu_bar.h
    views/message_dialog.cpp
    views/message_dialog.h
    views/performance_overlay.cpp
    views/performance_overlay.h
    views/popup_menu.cpp
    views/popup_menu.h
    views/rml_events.cpp
    views/rml_events.h
    views/dialog.cpp
    views/dialog.h
    views/game_details_panel.cpp
    views/game_details_panel.h
    views/general_dialog.cpp
    views/general_dialog.h
    views/resolution_scales.h
    views/scaling_filters.h
    views/settings_context.cpp
    views/settings_context.h
    views/settings_controls.cpp
    views/settings_controls.h
    views/settings_page.h
    views/user_profiles.cpp
    views/user_profiles.h
    views/volume_control.cpp
    views/volume_control.h
    views/about_dialog.cpp
    views/about_dialog.h
    views/advanced_dialog.cpp
    views/advanced_dialog.h
    views/controller_dialog.cpp
    views/controller_dialog.h
    views/controller_picture.cpp
    views/controller_picture.h
    views/directories_dialog.cpp
    views/directories_dialog.h
    views/firmware_dialog.cpp
    views/firmware_dialog.h
    views/shader_profile_dialog.cpp
    views/shader_profile_dialog.h
    views/hotkeys.cpp
    views/hotkeys.h
    views/loading_screen.cpp
    views/loading_screen.h
    views/profiles_dialog.cpp
    views/profiles_dialog.h
    views/shortcut.cpp
    views/shortcut.h
    views/tooltip.cpp
    views/tooltip.h
    "${_nxemu_rmlui_backends}/RmlUi_Platform_SDL.cpp"
)

# The interface's shaders are compiled to SPIR-V headers at build time.
find_program(GLSLANGVALIDATOR "glslangValidator" HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
set(_nxemu_ui_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/ui_shaders")
foreach(_nxemu_ui_shader
        blend_mask.frag blur.frag color.frag color_matrix.frag drop_shadow.frag
        fullscreen.vert geometry.vert gradient.frag passthrough.frag texture.frag)
    string(REPLACE "." "_" _nxemu_ui_shader_name "${_nxemu_ui_shader}")
    set(_nxemu_ui_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/host/shaders/${_nxemu_ui_shader}")
    set(_nxemu_ui_shader_header "${_nxemu_ui_shader_dir}/${_nxemu_ui_shader_name}_spv.h")
    add_custom_command(OUTPUT "${_nxemu_ui_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_nxemu_ui_shader_dir}"
        COMMAND "${GLSLANGVALIDATOR}" -V --quiet --variable-name "${_nxemu_ui_shader_name}_spv"
            -o "${_nxemu_ui_shader_header}" "${_nxemu_ui_shader_source}"
        MAIN_DEPENDENCY "${_nxemu_ui_shader_source}" VERBATIM)
    target_sources(nxemu_ui PRIVATE "${_nxemu_ui_shader_header}")
endforeach()

target_include_directories(nxemu_ui PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    "${_nxemu_rmlui_backends}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu"
    "${CMAKE_SOURCE_DIR}/src/nxemu-core"
    "${CMAKE_SOURCE_DIR}/src/nxemu-module-spec"
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/src/yuzu_common"
    "${CMAKE_SOURCE_DIR}/external/boost")
target_compile_definitions(nxemu_ui PRIVATE RMLUI_SDL_VERSION_MAJOR=2)
if(UNIX)
    target_compile_definitions(nxemu_ui PRIVATE NXEMU_UNIX=1)
endif()
target_link_libraries(nxemu_ui
    PUBLIC RmlUi::RmlUi SDL2::SDL2
    PRIVATE nxemu_core common yuzu_common discord-rpc fmt::fmt stb::headers Freetype::Freetype
        Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

if(APPLE)
    find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_sources(nxemu_ui PRIVATE platform/file_dialogs_macos.mm platform/platform_macos.mm)
    target_link_libraries(nxemu_ui PRIVATE "${CORETEXT_FRAMEWORK}" "${APPKIT_FRAMEWORK}" "${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}")
elseif(WIN32)
    target_link_libraries(nxemu_ui PRIVATE comdlg32 ole32 shell32 advapi32)
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS dbus-1 REQUIRED IMPORTED_TARGET)
    target_link_libraries(nxemu_ui PRIVATE PkgConfig::DBUS)
    find_package(Fontconfig QUIET)
    if(Fontconfig_FOUND)
        target_link_libraries(nxemu_ui PRIVATE Fontconfig::Fontconfig)
        target_compile_definitions(nxemu_ui PRIVATE NXEMU_HAVE_FONTCONFIG=1)
    endif()
endif()
