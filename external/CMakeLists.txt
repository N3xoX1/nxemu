# fmt
if (NOT TARGET fmt::fmt)
    add_subdirectory(fmt)
endif()

# The GameCube adapter driver is built from source: on macOS against the
# application deployment target, on Windows with the application's runtime.
if(ENABLE_LIBUSB AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TARGET libusb::usb)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NXEMU_LIBUSB REQUIRED IMPORTED_TARGET GLOBAL libusb-1.0)
    add_library(libusb::usb ALIAS PkgConfig::NXEMU_LIBUSB)
endif()
if(ENABLE_LIBUSB AND NOT TARGET libusb::usb)
    set(_nxemu_usb "${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb")
    add_library(nxemu_libusb STATIC
        ${_nxemu_usb}/core.c
        ${_nxemu_usb}/descriptor.c
        ${_nxemu_usb}/hotplug.c
        ${_nxemu_usb}/io.c
        ${_nxemu_usb}/strerror.c
        ${_nxemu_usb}/sync.c)
    target_include_directories(nxemu_libusb PUBLIC "${_nxemu_usb}")
    if(APPLE)
        target_sources(nxemu_libusb PRIVATE
            ${_nxemu_usb}/os/events_posix.c
            ${_nxemu_usb}/os/threads_posix.c
            ${_nxemu_usb}/os/darwin_usb.c)
        target_include_directories(nxemu_libusb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libusb/Xcode")
        target_compile_options(nxemu_libusb PRIVATE -fvisibility=hidden)
        target_link_libraries(nxemu_libusb PRIVATE Threads::Threads
            "-framework IOKit" "-framework CoreFoundation" "-framework Security")
    elseif(WIN32)
        target_sources(nxemu_libusb PRIVATE
            ${_nxemu_usb}/os/events_windows.c
            ${_nxemu_usb}/os/threads_windows.c
            ${_nxemu_usb}/os/windows_common.c
            ${_nxemu_usb}/os/windows_usbdk.c
            ${_nxemu_usb}/os/windows_winusb.c)
        target_include_directories(nxemu_libusb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libusb/msvc")
    endif()
    add_library(libusb::usb ALIAS nxemu_libusb)
    unset(_nxemu_usb)
endif()

# Linux packages provide the SDL runtime; macOS/Windows ship a bundled copy.
# SDL is built from source so the application ships its own copy.
if(ENABLE_SDL2 AND NOT TARGET SDL2::SDL2)
    # The frontend and the dynamically loaded OS module both use SDL. They
    # share one runtime: two copies would each claim the controllers, and
    # on macOS would define the same Objective-C classes twice.
    set(SDL_SHARED ON CACHE BOOL "Build shared SDL" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "Build static SDL" FORCE)
    set(SDL_TEST OFF CACHE BOOL "Build SDL test library" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "Build SDL tests" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "Disable SDL installation" FORCE)
    # SDL's bundled checks use an older minimum; CMake 4 requires at least 3.5.
    set(_nxemu_saved_policy_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
    if(NOT CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS "3.5")
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
    endif()
    add_subdirectory(sdl)
    set(CMAKE_POLICY_VERSION_MINIMUM "${_nxemu_saved_policy_minimum}")
    unset(_nxemu_saved_policy_minimum)
    if(WIN32)
        # Windows finds the library beside the executable that loads it.
        nxemu_set_app_output(SDL2)
        set_target_properties(SDL2 PROPERTIES DEBUG_POSTFIX "")
    endif()
endif()

# Boost
if(NOT TARGET Boost::headers)
    set(BOOST_SUPERPROJECT_VERSION "1.84.0" CACHE STRING "Boost version string for modular CMakeLists")

    set(_NXEMU_BOOST_ROOT "${CMAKE_SOURCE_DIR}/external/boost")

    add_library(boost_headers INTERFACE)
    add_library(Boost::headers ALIAS boost_headers)
    target_include_directories(boost_headers INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_library(boost_static_assert INTERFACE)
    add_library(Boost::static_assert ALIAS boost_static_assert)
    target_include_directories(boost_static_assert INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_library(boost_throw_exception INTERFACE)
    add_library(Boost::throw_exception ALIAS boost_throw_exception)
    target_include_directories(boost_throw_exception INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_library(boost_mp11 INTERFACE)
    add_library(Boost::mp11 ALIAS boost_mp11)
    target_include_directories(boost_mp11 INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_library(boost_pool INTERFACE)
    add_library(Boost::pool ALIAS boost_pool)
    target_include_directories(boost_pool INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_library(boost_smart_ptr INTERFACE)
    add_library(Boost::smart_ptr ALIAS boost_smart_ptr)
    target_include_directories(boost_smart_ptr INTERFACE "${_NXEMU_BOOST_ROOT}")

    add_subdirectory("${_NXEMU_BOOST_ROOT}/libs/config" "${CMAKE_BINARY_DIR}/external/boost/libs/config")
    add_subdirectory("${_NXEMU_BOOST_ROOT}/libs/assert" "${CMAKE_BINARY_DIR}/external/boost/libs/assert")
    add_subdirectory("${_NXEMU_BOOST_ROOT}/libs/core" "${CMAKE_BINARY_DIR}/external/boost/libs/core")
    add_subdirectory("${_NXEMU_BOOST_ROOT}/libs/predef" "${CMAKE_BINARY_DIR}/external/boost/libs/predef")
    add_subdirectory("${_NXEMU_BOOST_ROOT}/libs/context" "${CMAKE_BINARY_DIR}/external/boost/libs/context")
    if(MSVC)
        # The generic ASM language that nxemu-cpu and nxemu-video enable also
        # claims .asm files, and Visual Studio then leaves them out of the build.
        file(GLOB _nxemu_boost_masm "${_NXEMU_BOOST_ROOT}/libs/context/src/asm/*_masm.asm")
        set_source_files_properties(${_nxemu_boost_masm}
            DIRECTORY "${_NXEMU_BOOST_ROOT}/libs/context"
            PROPERTIES LANGUAGE ASM_MASM)
        unset(_nxemu_boost_masm)
    endif()
endif()

# Oaknut
if (ARCHITECTURE_arm64 AND NOT TARGET merry::oaknut)
    add_subdirectory(oaknut)
endif()

# stb (header-only; yuzu_common uses stb::headers)
if(NOT TARGET stb::headers)
    add_library(stb_headers INTERFACE)
    add_library(stb::headers ALIAS stb_headers)
    target_include_directories(stb_headers INTERFACE "${CMAKE_SOURCE_DIR}/src/3rd_party/stb")
endif()

if(NOT TARGET stb_dxt)
    add_library(stb_dxt STATIC "${CMAKE_SOURCE_DIR}/src/3rd_party/stb/stb_dxt.cpp")
    target_include_directories(stb_dxt PUBLIC "${CMAKE_SOURCE_DIR}/src/3rd_party/stb")
endif()

# zstd (yuzu_common links zstd::zstd; upstream exports libzstd_static as zstd::libzstd_static)
if(NOT TARGET libzstd_static)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Build zstd CLI" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Build zstd tests" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Build zstd contrib" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/zstd/build/cmake" "${CMAKE_BINARY_DIR}/external/zstd")
endif()
if(TARGET libzstd_static AND NOT TARGET zstd::zstd)
    add_library(zstd::zstd ALIAS libzstd_static)
endif()

# zlib + minizip (nxemu-loader firmware zip extraction)
# The bundled zlib 1.3.1 CMake temporarily renames its source zconf.h during
# out-of-source configuration. Restoring that file immediately makes quoted
# includes select the unconfigured source header on macOS, so POSIX prototypes
# such as read/write/lseek are lost. Use the SDK zlib on Apple instead.
if(APPLE)
    find_package(ZLIB REQUIRED)
elseif(NOT TARGET zlibstatic)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib example/minigzip programs" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/zlib" "${CMAKE_BINARY_DIR}/external/zlib")
    # Preserve the existing non-Apple behavior.
    set(_nxemu_zlib_src "${CMAKE_SOURCE_DIR}/external/zlib")
    if(EXISTS "${_nxemu_zlib_src}/zconf.h.included" AND NOT EXISTS "${_nxemu_zlib_src}/zconf.h")
        file(RENAME "${_nxemu_zlib_src}/zconf.h.included" "${_nxemu_zlib_src}/zconf.h")
    endif()
    unset(_nxemu_zlib_src)
endif()
if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

set(MINIZIP_DIR "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip")
add_library(minizip STATIC
    ${MINIZIP_DIR}/ioapi.c
    ${MINIZIP_DIR}/unzip.c
)
if(WIN32)
    target_sources(minizip PRIVATE ${MINIZIP_DIR}/iowin32.c)
endif()
target_include_directories(minizip PUBLIC
    ${CMAKE_SOURCE_DIR}/external/zlib
    ${MINIZIP_DIR}
)
target_link_libraries(minizip PUBLIC ZLIB::ZLIB)
set_property(TARGET minizip PROPERTY POSITION_INDEPENDENT_CODE ON)
if(NOT TARGET minizip::minizip)
    add_library(minizip::minizip ALIAS minizip)
endif()

# ENet
if (NOT TARGET enet::enet)
    add_subdirectory(enet)
    target_include_directories(enet INTERFACE ./enet/include)
    add_library(enet::enet ALIAS enet)
endif()

# Sirit
add_subdirectory(sirit)
nxemu_apply_dependency_cache_settings(sirit)

# httplib
if (ENABLE_WEB_SERVICE AND NOT TARGET httplib::httplib)
    set(HTTPLIB_REQUIRE_OPENSSL ON)
    add_subdirectory(cpp-httplib)
endif()

# Opus
if (NOT TARGET Opus::opus)
    # AArch64 always provides NEON. Opus 1.5.x does not recognize macOS'
    # "arm64" processor name as its "aarch64" presume-NEON case, while
    # runtime CPU capability detection is unavailable on this platform.
    if (APPLE AND ARCHITECTURE_arm64)
        set(OPUS_MAY_HAVE_NEON OFF CACHE BOOL "Use runtime NEON detection" FORCE)
        set(OPUS_PRESUME_NEON ON CACHE BOOL "Assume NEON support" FORCE)
    endif()
    if(MSVC)
        set(OPUS_STATIC_RUNTIME ON CACHE BOOL "Opus: static C runtime" FORCE)
    endif()
    add_subdirectory(opus)
    if (APPLE AND ARCHITECTURE_arm64)
        # Opus also guards NEON declarations with MAY_HAVE, even when presumed.
        target_compile_definitions(opus PRIVATE
            OPUS_ARM_MAY_HAVE_NEON OPUS_ARM_MAY_HAVE_NEON_INTR)
    endif()
endif()

# Vulkan — headers must come first: VulkanUtilityLibraries runs find_package(VulkanHeaders CONFIG),
# which resolves to the in-tree Vulkan::Headers target once vulkan-headers is added.
add_subdirectory(vulkan-headers)
add_subdirectory(vulkan_utility_libraries)
add_subdirectory(vulkan-memory-allocator)

# mcl
if (NOT TARGET merry::mcl)
    add_subdirectory(mcl)
    nxemu_apply_dependency_cache_settings(mcl)
endif()

# libadrenotools — Adreno BCn patching, turbo mode (Android arm64 only; see upstream CMake)
if(ANDROID AND ARCHITECTURE_arm64)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/libadrenotools/CMakeLists.txt")
        message(FATAL_ERROR
            "libadrenotools is missing (required for Android arm64). From the repo root run:\n"
            "  git submodule update --init --recursive external/libadrenotools")
    endif()
    add_subdirectory(libadrenotools)
endif()

# xbyak
if(ARCHITECTURE_x86_64 AND NOT TARGET xbyak::xbyak)
    add_subdirectory(xbyak)
endif()

# Zydis
if(ARCHITECTURE_x86_64)
    if(NOT TARGET Zydis)
        set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        add_subdirectory(zydis)
    endif()
    if(TARGET Zydis AND NOT TARGET zydis)
        add_library(zydis ALIAS Zydis)
    endif()
endif()

# robin-map
if(NOT TARGET tsl::robin_map)
    add_library(robin_map INTERFACE)
    add_library(tsl::robin_map ALIAS robin_map)
    target_include_directories(robin_map INTERFACE "${CMAKE_SOURCE_DIR}/external/robin-map/include")
endif()

# MoltenVK runtime (macOS)
if(APPLE)
    include(ExternalProject)

    find_program(NXEMU_XCODEBUILD xcodebuild REQUIRED)
    execute_process(COMMAND "${NXEMU_XCODEBUILD}" -version
        RESULT_VARIABLE _nxemu_xcode_result OUTPUT_QUIET ERROR_VARIABLE _nxemu_xcode_error)
    if(NOT _nxemu_xcode_result EQUAL 0)
        message(FATAL_ERROR
            "Building MoltenVK requires full Xcode, not only Command Line Tools. "
            "Select Xcode with xcode-select. ${_nxemu_xcode_error}")
    endif()

    set(NXEMU_MOLTENVK_PREBUILT "" CACHE FILEPATH "Prebuilt MoltenVK runtime for this toolchain")
    if(NXEMU_MOLTENVK_PREBUILT)
        if(NOT EXISTS "${NXEMU_MOLTENVK_PREBUILT}")
            message(FATAL_ERROR "Prebuilt MoltenVK runtime not found: ${NXEMU_MOLTENVK_PREBUILT}")
        endif()
        message(STATUS "Using prebuilt MoltenVK: ${NXEMU_MOLTENVK_PREBUILT}")
        set(NXEMU_MOLTENVK_RUNTIME "${NXEMU_MOLTENVK_PREBUILT}" CACHE INTERNAL "" FORCE)
    else()
        set(_nxemu_moltenvk_root "${CMAKE_BINARY_DIR}/external/moltenvk")
        set(_nxemu_moltenvk_source "${_nxemu_moltenvk_root}/src")
        set(_nxemu_moltenvk_runtime
            "${_nxemu_moltenvk_source}/MoltenVK/Package/Latest/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib")

        ExternalProject_Add(nxemu_moltenvk
            GIT_REPOSITORY "https://github.com/V380-Ori/Ryujinx.MoltenVK.git"
            GIT_TAG "45f9b641e45f1d150f3521d74808477add9e975b"
            GIT_SHALLOW FALSE
            GIT_SUBMODULES_RECURSE TRUE
            UPDATE_COMMAND ""
            SOURCE_DIR "${_nxemu_moltenvk_source}"
            CONFIGURE_COMMAND ""
            BUILD_IN_SOURCE TRUE
            BUILD_COMMAND /bin/sh -c
                "cd MoltenVK && (git apply --reverse --check ../000*.patch || git apply --verbose ../000*.patch) && ./fetchDependencies --macos && make macos"
            BUILD_BYPRODUCTS "${_nxemu_moltenvk_runtime}"
            INSTALL_COMMAND ""
        )

        set(NXEMU_MOLTENVK_RUNTIME "${_nxemu_moltenvk_runtime}" CACHE INTERNAL "" FORCE)
        unset(_nxemu_moltenvk_root)
        unset(_nxemu_moltenvk_source)
        unset(_nxemu_moltenvk_runtime)
    endif()
endif()

# Interface: RmlUi, with FreeType as its font engine. Both are built from
# source so every platform renders text with the same library.
if(NOT ANDROID)
    set(BUILD_SHARED_LIBS OFF)

    foreach(_nxemu_ft_dependency ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${_nxemu_ft_dependency} ON CACHE BOOL "FreeType: no external dependency" FORCE)
    endforeach()
    unset(_nxemu_ft_dependency)
    set(SKIP_INSTALL_ALL ON)
    # FreeType's minimum predates what CMake 4 accepts.
    set(_nxemu_saved_policy_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
    if(NOT CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS "3.5")
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
    endif()
    add_subdirectory(freetype)
    set(CMAKE_POLICY_VERSION_MINIMUM "${_nxemu_saved_policy_minimum}")
    unset(_nxemu_saved_policy_minimum)
    unset(SKIP_INSTALL_ALL)
    add_library(Freetype::Freetype ALIAS freetype)

    # RmlUi takes the target above instead of searching the system.
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype TRUE)
    set(_nxemu_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    nxemu_enable_rtti()
    add_subdirectory(rmlui)
    set(CMAKE_CXX_FLAGS "${_nxemu_saved_cxx_flags}")
    unset(_nxemu_saved_cxx_flags)
    unset(CMAKE_DISABLE_FIND_PACKAGE_Freetype)
endif()
