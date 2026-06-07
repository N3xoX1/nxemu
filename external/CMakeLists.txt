# fmt
if (NOT TARGET fmt::fmt)
    add_subdirectory(fmt)
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
endif()

# Oaknut
if (ARCHITECTURE_arm64 AND NOT TARGET merry::oaknut)
    add_subdirectory(oaknut)
endif()

# MicroProfile (vendored under src/3rd_party; external/microprofile is not used)
add_library(microprofile INTERFACE)
target_include_directories(microprofile INTERFACE "${CMAKE_SOURCE_DIR}/src/3rd_party/microprofile")

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
if(NOT TARGET zlibstatic)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/zlib" "${CMAKE_BINARY_DIR}/external/zlib")
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

# httplib
if (ENABLE_WEB_SERVICE AND NOT TARGET httplib::httplib)
    set(HTTPLIB_REQUIRE_OPENSSL ON)
    add_subdirectory(cpp-httplib)
endif()

# Opus
if (NOT TARGET Opus::opus)
    add_subdirectory(opus)
endif()

# Vulkan — headers must come first: VulkanUtilityLibraries runs find_package(VulkanHeaders CONFIG),
# which resolves to the in-tree Vulkan::Headers target once vulkan-headers is added.
add_subdirectory(vulkan-headers)
add_subdirectory(vulkan_utility_libraries)
add_subdirectory(vulkan-memory-allocator)

# mcl
if (NOT TARGET merry::mcl)
    add_subdirectory(mcl)
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
