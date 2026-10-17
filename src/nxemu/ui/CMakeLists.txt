set(_nxemu_rmlui_backends "${CMAKE_SOURCE_DIR}/external/rmlui/Backends")

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED CONFIG)
endif()

# The RmlUi frontend. Everything here is shared by all platforms, apart from
# the few window conventions macOS needs set natively.
add_library(nxemu_ui STATIC
    ui_application.cpp
    ui_application.h
    emulation/emulator_session.cpp
    emulation/emulator_session.h
    host/shell.cpp
    host/shell.h
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
    host/vulkan/vma.cpp
    host/vulkan/vma.h
    host/vulkan/vulkan_api.cpp
    host/vulkan/vulkan_api.h
    i18n/plural_rule.cpp
    i18n/plural_rule.h
    i18n/translation.cpp
    i18n/translation.h
    platform/native_window.h
    views/main_window.cpp
    views/main_window.h
    views/menu_bar.cpp
    views/menu_bar.h
    views/popup_menu.cpp
    views/popup_menu.h
    views/rml_events.cpp
    views/rml_events.h
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
    PRIVATE nxemu_core common yuzu_common fmt::fmt stb::headers Freetype::Freetype
        Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

if(APPLE)
    find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
    target_sources(nxemu_ui PRIVATE platform/platform_macos.mm)
    target_link_libraries(nxemu_ui PRIVATE "${CORETEXT_FRAMEWORK}")
elseif(UNIX)
    find_package(Fontconfig QUIET)
    if(Fontconfig_FOUND)
        target_link_libraries(nxemu_ui PRIVATE Fontconfig::Fontconfig)
        target_compile_definitions(nxemu_ui PRIVATE NXEMU_HAVE_FONTCONFIG=1)
    endif()
endif()
