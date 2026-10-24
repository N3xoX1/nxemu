set(_nxemu_rmlui_backends "${CMAKE_SOURCE_DIR}/external/rmlui/Backends")

add_library(nxemu-android SHARED
    android_render_window.cpp
    emulation_session.cpp
    native.cpp
    native_input.cpp
    notification.cpp
    settings/settings_bridge.cpp
    settings/ui_settings.cpp
    ui/ui_activity.cpp
    ui/ui_activity.h
    ui/ui_files.cpp
    ui/ui_files.h
    ui/ui_host.cpp
    ui/ui_host.h
    ui/ui_jni.cpp
    ui/ui_library.cpp
    ui/ui_library.h
    ui/ui_renderer.cpp
    ui/ui_renderer.h
    ui/ui_screen.cpp
    ui/ui_screen.h
    ui/ui_settings_pages.cpp
    ui/ui_thread.cpp
    ui/ui_thread.h
    ui/ui_util.cpp
    ui/ui_util.h
    "${_nxemu_rmlui_backends}/RmlUi_Renderer_GL3.cpp"
)

target_include_directories(nxemu-android PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${_nxemu_rmlui_backends}"
)

target_link_libraries(nxemu-android PRIVATE
    nxemu_core
    yuzu_common
    common
    RmlUi::RmlUi
    android
    EGL
    GLESv3
    log
)
