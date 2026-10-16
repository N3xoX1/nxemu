option(NXEMU_MACOS_BUNDLE "Build NxEmu as a macOS application bundle" ON)

# The interface derives from RmlUi's classes.
nxemu_enable_rtti()

add_subdirectory(ui)

add_executable(nxemu
    main.cpp
    startup_checks.cpp
    startup_checks.h
    vulkan_probe.cpp
    vulkan_probe.h
)

target_include_directories(nxemu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/nxemu
    ${CMAKE_SOURCE_DIR}/src/nxemu-core
    ${CMAKE_SOURCE_DIR}/src/nxemu-module-spec
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/yuzu_common
    ${CMAKE_SOURCE_DIR}/external/boost
)

if(UNIX)
    target_compile_definitions(nxemu PRIVATE NXEMU_UNIX=1)
endif()

if(WIN32)
    # A windowed application: no console, and the icon and version details
    # Explorer shows.
    set_target_properties(nxemu PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(nxemu PRIVATE windows/nxemu.rc)
endif()

target_link_libraries(nxemu PRIVATE
    nxemu_ui
    common
    yuzu_common
    nxemu_core
    fmt::fmt
    Vulkan::Headers
    Vulkan::UtilityHeaders
    ${CMAKE_DL_LIBS}
)
nxemu_set_app_output(nxemu)
add_dependencies(nxemu nxemu-loader nxemu-cpu nxemu-video nxemu-os)

set(_nxemu_ui_assets "${CMAKE_CURRENT_SOURCE_DIR}/ui/assets")
file(GLOB _nxemu_ui_catalogues CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/lang/*.po")
# Updating a translation also refreshes the files beside the executable.
set_property(TARGET nxemu APPEND PROPERTY LINK_DEPENDS ${_nxemu_ui_catalogues})

if(APPLE)
    if(NOT NXEMU_MOLTENVK_RUNTIME)
        message(FATAL_ERROR "MoltenVK runtime was not configured")
    endif()
    if(TARGET nxemu_moltenvk)
        add_dependencies(nxemu nxemu_moltenvk)
    endif()
endif()

if(APPLE AND NXEMU_MACOS_BUNDLE)
    set(_nxemu_icon "${CMAKE_CURRENT_SOURCE_DIR}/macos/nxemu.icns")
    set_target_properties(nxemu PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.nxemu"
        MACOSX_BUNDLE_BUNDLE_NAME "NxEmu"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_BUILD}"
        OUTPUT_NAME "NxEmu"
        BUILD_RPATH "@executable_path/../Frameworks")
    find_program(NXEMU_CODESIGN codesign REQUIRED)
    find_program(NXEMU_INSTALL_NAME_TOOL install_name_tool REQUIRED)
    find_program(NXEMU_OTOOL otool REQUIRED)
    set(_nxemu_bundle_script "${CMAKE_SOURCE_DIR}/src/cmake/modules/nxemu_macos_bundle.cmake")
    set(_nxemu_bundle_command ${CMAKE_COMMAND}
        "-DBUNDLE=$<TARGET_BUNDLE_DIR:nxemu>"
        "-DEXECUTABLE=$<TARGET_FILE:nxemu>"
        "-DLOADER=$<TARGET_FILE:nxemu-loader>"
        "-DCPU=$<TARGET_FILE:nxemu-cpu>"
        "-DVIDEO=$<TARGET_FILE:nxemu-video>"
        "-DOS=$<TARGET_FILE:nxemu-os>"
        "-DMOLTENVK=${NXEMU_MOLTENVK_RUNTIME}"
        "-DSDL=$<TARGET_FILE:SDL2>"
        "-DASSETS=${_nxemu_ui_assets}"
        "-DCODESIGN=${NXEMU_CODESIGN}"
        "-DINSTALL_NAME_TOOL=${NXEMU_INSTALL_NAME_TOOL}"
        "-DOTOOL=${NXEMU_OTOOL}"
        "-DINFO_PLIST=${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        "-DICON=${_nxemu_icon}"
        "-DSHORT_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}"
        "-DBUILD_VERSION=${VERSION_BUILD}"
        "-DMINIMUM_MACOS=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        -P "${_nxemu_bundle_script}")
    # Module-only rebuilds must refresh the app even when its own code did not change.
    # LINK_DEPENDS also covers an explicit --target nxemu build.
    set_property(TARGET nxemu APPEND PROPERTY LINK_DEPENDS
        "$<TARGET_FILE:nxemu-loader>" "$<TARGET_FILE:nxemu-cpu>"
        "$<TARGET_FILE:nxemu-video>" "$<TARGET_FILE:nxemu-os>"
        "${NXEMU_MOLTENVK_RUNTIME}" "${_nxemu_bundle_script}"
        "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in" "${_nxemu_icon}")
    add_custom_command(TARGET nxemu POST_BUILD
        COMMAND ${_nxemu_bundle_command} VERBATIM
        COMMENT "Package and sign NxEmu.app with this build's modules")
    add_custom_target(nxemu_bundle ALL
        COMMAND ${_nxemu_bundle_command}
        DEPENDS nxemu
        VERBATIM
        COMMENT "Refresh NxEmu.app modules and interface files")
else()
    include(nxemu_setup_environment)
    nxemu_setup_environment(nxemu)
    foreach(_module nxemu-loader nxemu-cpu nxemu-video nxemu-os)
        if(_module STREQUAL "nxemu-os")
            set(_folder operating_system)
        else()
            string(REPLACE "nxemu-" "" _folder "${_module}")
        endif()
        set_property(TARGET nxemu APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:${_module}>")
        add_custom_command(TARGET nxemu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxemu>/modules/${_folder}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_module}>" "$<TARGET_FILE_DIR:nxemu>/modules/${_folder}"
            VERBATIM)
    endforeach()
    # The interface's documents, fonts and images ship beside the executable.
    add_custom_command(TARGET nxemu POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_nxemu_ui_assets}" "$<TARGET_FILE_DIR:nxemu>/ui"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxemu>/ui/lang"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_nxemu_ui_catalogues} "$<TARGET_FILE_DIR:nxemu>/ui/lang"
        VERBATIM)
    if(APPLE)
        add_custom_command(TARGET nxemu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NXEMU_MOLTENVK_RUNTIME}" "$<TARGET_FILE_DIR:nxemu>/libMoltenVK.dylib"
            VERBATIM)
    endif()
endif()
