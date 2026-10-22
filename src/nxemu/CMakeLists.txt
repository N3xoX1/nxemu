option(NXEMU_MACOS_BUNDLE "Build NxEmu as a macOS application bundle" ON)

add_subdirectory(ui)

add_executable(nxemu
    main.cpp
    startup_checks.cpp
    startup_checks.h
    vulkan_probe.cpp
    vulkan_probe.h
    settings/ui_settings.cpp
    settings/ui_settings.h
    settings/ui_identifiers.h
)

file(GLOB _nxemu_ui_catalogues CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/lang/*.po")

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

target_link_libraries(nxemu PRIVATE
    nxemu_ui
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
        BUILD_RPATH "@executable_path/../Frameworks"
    )
    if(NOT NXEMU_MOLTENVK_RUNTIME)
        message(FATAL_ERROR "The macOS bundle requires MoltenVK")
    endif()
    if(TARGET nxemu_moltenvk)
        add_dependencies(nxemu nxemu_moltenvk)
    endif()
    add_dependencies(nxemu nxemu-loader nxemu-cpu nxemu-video nxemu-os)
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
        "-DSDL=$<TARGET_FILE:SDL2::SDL2>"
        "-DASSETS=${CMAKE_CURRENT_SOURCE_DIR}/ui/assets"
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
        COMMENT "Refresh NxEmu.app runtime and language resources")
elseif(APPLE)
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
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui/assets" "$<TARGET_FILE_DIR:nxemu>/ui"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxemu>/ui/lang"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_nxemu_ui_catalogues} "$<TARGET_FILE_DIR:nxemu>/ui/lang"
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
