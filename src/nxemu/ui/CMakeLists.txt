set(_nxemu_rmlui_backends "${CMAKE_SOURCE_DIR}/external/rmlui/Backends")

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED CONFIG)
endif()

# The NxEmu frontend. Everything here is shared by all platforms, apart from
# the few window conventions macOS needs set natively.
add_library(nxemu_ui STATIC
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
    host/ui_renderer.cpp
    host/ui_renderer.h
    host/ui_renderer_effects.cpp
    host/ui_renderer_internal.h
    host/ui_window.cpp
    host/ui_window.h
    host/ui_theme.cpp
    host/ui_theme.h
    host/vulkan/vma.cpp
    host/vulkan/vma.h
    host/vulkan/vulkan_api.cpp
    host/vulkan/vulkan_api.h
    i18n/plural_rule.cpp
    i18n/plural_rule.h
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
    library/game_cache.cpp
    library/game_details.cpp
    library/game_details.h
    library/game_library.cpp
    library/game_library.h
    library/game_library_cache.cpp
    library/game_library_cache.h
    library/library_files.cpp
    library/library_files.h
    library/play_time.cpp
    library/play_time.h
    library/rom_properties.cpp
    library/rom_properties.h
    library/ui_preferences.cpp
    library/ui_preferences.h
    platform/file_dialogs.cpp
    platform/file_dialogs.h
    platform/native_window.h
    platform/system_theme.cpp
    platform/system_theme.h
    views/about_dialog.cpp
    views/about_dialog.h
    views/advanced_dialog.cpp
    views/advanced_dialog.h
    views/controller_dialog.cpp
    views/controller_dialog.h
    views/controller_picture.cpp
    views/controller_picture.h
    views/dialog.cpp
    views/dialog.h
    views/directories_dialog.cpp
    views/directories_dialog.h
    views/firmware_dialog.cpp
    views/firmware_dialog.h
    views/general_dialog.cpp
    views/general_dialog.h
    views/game_details_panel.cpp
    views/game_details_panel.h
    views/loading_screen.cpp
    views/loading_screen.h
    views/main_window.cpp
    views/main_window.h
    views/menu_bar.cpp
    views/menu_bar.h
    views/performance_overlay.cpp
    views/performance_overlay.h
    views/popup_menu.cpp
    views/popup_menu.h
    views/profiles_dialog.cpp
    views/profiles_dialog.h
    views/rml_events.cpp
    views/rml_events.h
    views/settings_context.cpp
    views/settings_context.h
    views/settings_controls.cpp
    views/settings_controls.h
    views/settings_page.h
    views/hotkeys.cpp
    views/hotkeys.h
    views/shortcut.cpp
    views/shortcut.h
    views/tooltip.cpp
    views/tooltip.h
    views/user_profiles.cpp
    views/user_profiles.h
    views/volume_control.cpp
    views/volume_control.h
    "${_nxemu_rmlui_backends}/RmlUi_Platform_SDL.cpp"
)

# The interface's shaders are compiled to SPIR-V headers at build time.
# On Windows it comes with the Vulkan SDK, which may not be on the PATH.
find_program(GLSLANGVALIDATOR "glslangValidator" HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
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
target_include_directories(nxemu_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_include_directories(nxemu_ui PRIVATE
    "${_nxemu_rmlui_backends}"
    "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(nxemu_ui PRIVATE
    RMLUI_SDL_VERSION_MAJOR=2
    NXEMU_UI_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(nxemu_ui PUBLIC RmlUi::RmlUi SDL2::SDL2
    PRIVATE nxemu_core common yuzu_common discord-rpc Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

option(NXEMU_BUILD_HOTKEY_TESTS "Build the keyboard and gamepad shortcut tests" OFF)
if(NXEMU_BUILD_HOTKEY_TESTS)
    enable_testing()
    add_executable(nxemu_hotkey_tests views/hotkeys_tests.cpp views/hotkeys.cpp views/shortcut.cpp)
    target_include_directories(nxemu_hotkey_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(nxemu_hotkey_tests PRIVATE SDL2::SDL2)
    add_test(NAME nxemu_hotkeys COMMAND nxemu_hotkey_tests)
endif()

option(NXEMU_BUILD_TRANSLATION_TESTS "Build the interface translation tests" OFF)
if(NXEMU_BUILD_TRANSLATION_TESTS)
    enable_testing()
    add_executable(nxemu_translation_tests
        i18n/translation_tests.cpp
        i18n/translation.cpp
        i18n/plural_rule.cpp
        host/system_fonts.cpp
        host/ui_theme.cpp
        library/ui_preferences.cpp)
    target_link_libraries(nxemu_translation_tests PRIVATE RmlUi::RmlUi nxemu_core common yuzu_common)
    add_test(NAME nxemu_translations COMMAND nxemu_translation_tests "${CMAKE_CURRENT_SOURCE_DIR}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME nxemu_translation_template COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/lang/extract_strings.py" --check)
    add_test(NAME nxemu_translation_catalogues COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/lang/check_translations.py")
endif()

option(NXEMU_BUILD_LIBRARY_CACHE_TESTS "Build the game library cache tests" OFF)
if(NXEMU_BUILD_LIBRARY_CACHE_TESTS)
    add_executable(nxemu_game_library_cache_tests
        library/game_library_cache_tests.cpp
        library/game_cache.cpp
        library/game_library_cache.cpp
        library/library_files.cpp)
    target_include_directories(nxemu_game_library_cache_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(nxemu_game_library_cache_tests PRIVATE common yuzu_common)
    enable_testing()
    add_test(NAME nxemu_game_library_cache COMMAND nxemu_game_library_cache_tests)
    add_executable(nxemu_game_library_scan_tests
        library/game_library_scan_tests.cpp
        i18n/translation.cpp
        i18n/plural_rule.cpp
        library/game_library.cpp
        library/game_library_cache.cpp
        library/library_files.cpp
        library/rom_properties.cpp
        library/game_details.cpp
        library/ui_preferences.cpp)
    target_include_directories(nxemu_game_library_scan_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(nxemu_game_library_scan_tests PRIVATE RmlUi::RmlUi nxemu_core common yuzu_common)
    add_test(NAME nxemu_game_library_scan COMMAND nxemu_game_library_scan_tests)
endif()

# On macOS the modules are built into the build tree; an executable run
# outside a bundle loads them from there.
if(APPLE)
    target_compile_definitions(nxemu_ui PRIVATE
        NXEMU_UI_BUILD_MODULE_DIR="${CMAKE_BINARY_DIR}/modules/${NXEMU_PLATFORM_DIR}/$<CONFIG>")
endif()

if(APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_sources(nxemu_ui PRIVATE
        platform/file_dialogs_macos.mm
        platform/platform_macos.mm)
    target_link_libraries(nxemu_ui PRIVATE "${APPKIT_FRAMEWORK}" "${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}")
    foreach(_nxemu_font_target nxemu_ui nxemu_translation_tests)
        if(TARGET ${_nxemu_font_target})
            target_link_libraries(${_nxemu_font_target} PRIVATE "${CORETEXT_FRAMEWORK}" Freetype::Freetype)
        endif()
    endforeach()
elseif(WIN32)
    target_link_libraries(nxemu_ui PRIVATE comdlg32 ole32 shell32 advapi32)
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS dbus-1 REQUIRED IMPORTED_TARGET)
    target_link_libraries(nxemu_ui PRIVATE ${CMAKE_DL_LIBS} PkgConfig::DBUS Threads::Threads)
    find_package(Fontconfig QUIET)
    if(Fontconfig_FOUND)
        foreach(_nxemu_font_target nxemu_ui nxemu_translation_tests)
            if(TARGET ${_nxemu_font_target})
                target_link_libraries(${_nxemu_font_target} PRIVATE Fontconfig::Fontconfig)
                target_compile_definitions(${_nxemu_font_target} PRIVATE NXEMU_HAVE_FONTCONFIG=1)
            endif()
        endforeach()
    endif()
endif()
