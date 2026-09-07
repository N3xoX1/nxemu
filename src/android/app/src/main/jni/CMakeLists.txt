add_library(nxemu-android SHARED
    android_render_window.cpp
    emulation_session.cpp
    native.cpp
    notification.cpp
    settings/settings_bridge.cpp
    settings/ui_settings.cpp
)

target_include_directories(nxemu-android PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nxemu-android PRIVATE
    nxemu_core
    yuzu_common
    common
    android
    log
)
