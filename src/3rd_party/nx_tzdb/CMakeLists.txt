# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Visual Studio generates these headers with nx_tzdb_create_header.exe.
# CMake (Android/Linux) generates them the same way yuzu does: cmake -P
# NxTzdbCreateHeader.cmake, writing into the build tree.

set(NX_TZDB_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${NX_TZDB_INCLUDE_DIR}/nx_tzdb")

add_library(nx_tzdb INTERFACE)

# Same layout the VS custom-build rules use.
set(NX_ZONEINFO_ROOT "${CMAKE_SOURCE_DIR}/src/3rd_party/zoneinfo")

if (EXISTS "${NX_ZONEINFO_ROOT}/Africa")
    set(NX_TZDB_BASE_DIR "${NX_ZONEINFO_ROOT}/Base")
    set(NX_TZDB_ZONEINFO_DIR "${NX_ZONEINFO_ROOT}")
else()
    # Vendored zoneinfo is incomplete. Fetch Switch-format timezone *data*
    # (not generated headers) so CreateHeader has the same inputs VS expects.
    set(NX_TZDB_VERSION "221202")
    set(NX_TZDB_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/${NX_TZDB_VERSION}.zip")
    set(NX_TZDB_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/nx_tzdb")

    if (NOT EXISTS "${NX_TZDB_ROMFS_DIR}/zoneinfo/Africa")
        set(NX_TZDB_DOWNLOAD_URL "https://github.com/lat9nq/tzdb_to_nx/releases/download/${NX_TZDB_VERSION}/${NX_TZDB_VERSION}.zip")
        message(STATUS "Downloading time zone data from ${NX_TZDB_DOWNLOAD_URL}...")
        file(DOWNLOAD ${NX_TZDB_DOWNLOAD_URL} ${NX_TZDB_ARCHIVE}
            STATUS NX_TZDB_DOWNLOAD_STATUS)
        list(GET NX_TZDB_DOWNLOAD_STATUS 0 NX_TZDB_DOWNLOAD_STATUS_CODE)
        if (NOT NX_TZDB_DOWNLOAD_STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Time zone data download failed (status code ${NX_TZDB_DOWNLOAD_STATUS_CODE})")
        endif()

        file(ARCHIVE_EXTRACT
            INPUT ${NX_TZDB_ARCHIVE}
            DESTINATION ${NX_TZDB_ROMFS_DIR})
    endif()

    set(NX_TZDB_BASE_DIR "${NX_TZDB_ROMFS_DIR}")
    set(NX_TZDB_ZONEINFO_DIR "${NX_TZDB_ROMFS_DIR}/zoneinfo")
endif()

target_include_directories(nx_tzdb
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
    INTERFACE ${NX_TZDB_INCLUDE_DIR})

function(CreateHeader ZONE_PATH HEADER_NAME)
    set(HEADER_PATH "${NX_TZDB_INCLUDE_DIR}/nx_tzdb/${HEADER_NAME}.h")
    add_custom_command(
        OUTPUT
            ${HEADER_PATH}
        COMMAND
            ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/NxTzdbCreateHeader.cmake
                ${ZONE_PATH}
                ${HEADER_NAME}
                ${NX_TZDB_INCLUDE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS
            tzdb_template.h.in
            NxTzdbCreateHeader.cmake
            ListFilesInDirectory.cmake)

    target_sources(nx_tzdb PRIVATE ${HEADER_PATH})
endfunction()

CreateHeader(${NX_TZDB_BASE_DIR} base)
CreateHeader(${NX_TZDB_ZONEINFO_DIR} zoneinfo)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Africa africa)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/America america)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/America/Argentina america_argentina)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/America/Indiana america_indiana)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/America/Kentucky america_kentucky)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/America/North_Dakota america_north_dakota)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Antarctica antarctica)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Arctic arctic)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Asia asia)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Atlantic atlantic)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Australia australia)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Brazil brazil)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Canada canada)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Chile chile)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Etc etc)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Europe europe)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Indian indian)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Mexico mexico)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/Pacific pacific)
CreateHeader(${NX_TZDB_ZONEINFO_DIR}/US us)
