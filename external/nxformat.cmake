# nxformat (https://github.com/fab918/nxformat): the library the NX dialogs
# use to convert games to .nx and edit .nx archives. Its CI publishes one
# archive per platform; the pinned one is downloaded and checked here.
# NXEMU_NXFORMAT_ROOT may instead point at an unpacked archive, such as a
# local build packed by nxformat's tools/package_release.py.
#
# Defines nxformat::nxformat and NXEMU_NXFORMAT_RUNTIME, the shared library
# to ship next to the executable.
set(NXEMU_NXFORMAT_ROOT "" CACHE PATH "Unpacked nxformat release (empty: download the pinned one)")

set(_nxformat_version 0.2.0)
set(_nxformat_root "${NXEMU_NXFORMAT_ROOT}")
if(NOT _nxformat_root)
    if(WIN32 AND ARCHITECTURE_x86_64)
        set(_nxformat_platform windows-x64)
        set(_nxformat_sha256 c53ec5a8b8befc42c46e431f937272468e79c21593f0d38d07835a631d83fd49)
    elseif(APPLE)
        set(_nxformat_platform macos-universal)
        set(_nxformat_sha256 02eeec5d40c1cac775fcfde49e67db12ddd5ca8a81470e4f73d72dfa018bb08f)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_x86_64)
        set(_nxformat_platform linux-x64)
        set(_nxformat_sha256 a6759cd759c0aa9c97525c75a8ac35286ececb82cf69df671ecef0de689acfbb)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_arm64)
        set(_nxformat_platform linux-arm64)
        set(_nxformat_sha256 aea6a065b3c3bbfb7ce1be3604aafc2e4df10e152f74cc20be1686292e5e8fa3)
    else()
        message(FATAL_ERROR "No nxformat build for this platform: set NXEMU_NXFORMAT_ROOT")
    endif()
    if(WIN32)
        set(_nxformat_ext zip)
    else()
        set(_nxformat_ext tar.gz)
    endif()
    set(_nxformat_name "nxformat-${_nxformat_version}-${_nxformat_platform}")
    set(_nxformat_archive "${CMAKE_BINARY_DIR}/external/${_nxformat_name}.${_nxformat_ext}")
    set(_nxformat_root "${CMAKE_BINARY_DIR}/external/${_nxformat_name}")
    if(NOT EXISTS "${_nxformat_root}/lib/cmake/nxformat/nxformatConfig.cmake")
        # Skipped when the archive is already there with this hash.
        file(DOWNLOAD
            "https://github.com/fab918/nxformat/releases/download/v${_nxformat_version}/${_nxformat_name}.${_nxformat_ext}"
            "${_nxformat_archive}"
            EXPECTED_HASH SHA256=${_nxformat_sha256}
            TLS_VERIFY ON
            STATUS _nxformat_status)
        list(GET _nxformat_status 0 _nxformat_code)
        if(NOT _nxformat_code EQUAL 0)
            list(GET _nxformat_status 1 _nxformat_message)
            message(FATAL_ERROR "Could not download ${_nxformat_name}: ${_nxformat_message}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_nxformat_archive}" DESTINATION "${_nxformat_root}")
    endif()
endif()

find_package(nxformat ${_nxformat_version} REQUIRED CONFIG
    PATHS "${_nxformat_root}/lib/cmake/nxformat" NO_DEFAULT_PATH)
set_target_properties(nxformat::nxformat PROPERTIES IMPORTED_GLOBAL TRUE)
set(NXEMU_NXFORMAT_RUNTIME "${NXFORMAT_RUNTIME}" CACHE INTERNAL "")
message(STATUS "nxformat ${nxformat_VERSION}: ${_nxformat_root}")

unset(_nxformat_version)
unset(_nxformat_root)
unset(_nxformat_platform)
unset(_nxformat_sha256)
unset(_nxformat_ext)
unset(_nxformat_name)
unset(_nxformat_archive)
unset(_nxformat_status)
unset(_nxformat_code)
unset(_nxformat_message)
