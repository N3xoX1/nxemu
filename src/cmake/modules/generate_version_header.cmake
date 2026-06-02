# Generate Version Header
#
# Reads version.h.in, extracts VERSION_MAJOR/MINOR/REVISION/PREFIX,
# queries git for build number, revision, and dirty status,
# then writes an updated version.h to the build directory.
#
# Usage:
#   set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-core/version.h.in")
#   set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/version.h")
#   include(GenerateVersionHeader)

if(NOT DEFINED VERSION_HEADER_IN)
    message(FATAL_ERROR "VERSION_HEADER_IN not set")
endif()
if(NOT DEFINED VERSION_HEADER_OUT)
    message(FATAL_ERROR "VERSION_HEADER_OUT not set")
endif()

# Read the .in file
file(READ "${VERSION_HEADER_IN}" VERSION_CONTENT)

# Parse existing version numbers from the #define lines
string(REGEX MATCH "#define VERSION_MAJOR[ \t]+([0-9]+)" _match "${VERSION_CONTENT}")
set(VERSION_MAJOR "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define VERSION_MINOR[ \t]+([0-9]+)" _match "${VERSION_CONTENT}")
set(VERSION_MINOR "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define VERSION_REVISION[ \t]+([0-9]+)" _match "${VERSION_CONTENT}")
set(VERSION_REVISION "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define VERSION_PREFIX[ \t]+\"([^\"]*)\"" _match "${VERSION_CONTENT}")
set(VERSION_PREFIX "${CMAKE_MATCH_1}")

# Defaults
set(VERSION_BUILD 9999)
set(GIT_REVISION "")
set(GIT_REVISION_SHORT "")
set(GIT_DIRTY "")
set(GIT_VERSION "Unknown")

# Query git
find_package(Git QUIET)
if(GIT_FOUND)
    # Build number = commit count
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_build
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _git_build)
        set(VERSION_BUILD "${_git_build}")
    endif()

    # Full revision hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Short revision hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_REVISION_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Dirty check
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff --stat
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_diff
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(_git_diff)
        set(GIT_DIRTY "Dirty")
        set(GIT_VERSION "${GIT_REVISION_SHORT}-Dirty")
    else()
        set(GIT_DIRTY "")
        set(GIT_VERSION "${GIT_REVISION_SHORT}")
    endif()
endif()

message(STATUS "NxEmu version: ${VERSION_PREFIX}${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}.${VERSION_BUILD}-${GIT_VERSION}")

string(TIMESTAMP VERSION_BUILD_YEAR "%Y" UTC)

# Replace the lines in the template
string(REGEX REPLACE
    "#define VERSION_BUILD_YEAR[ \t]+[0-9]+"
    "#define VERSION_BUILD_YEAR          ${VERSION_BUILD_YEAR}"
    VERSION_CONTENT "${VERSION_CONTENT}")

string(REGEX REPLACE
    "#define VERSION_BUILD[ \t]+[0-9]+"
    "#define VERSION_BUILD               ${VERSION_BUILD}"
    VERSION_CONTENT "${VERSION_CONTENT}")

string(REGEX REPLACE
    "#define GIT_REVISION[ \t]+\"[^\"]*\""
    "#define GIT_REVISION                \"${GIT_REVISION}\""
    VERSION_CONTENT "${VERSION_CONTENT}")

string(REGEX REPLACE
    "#define GIT_REVISION_SHORT[ \t]+\"[^\"]*\""
    "#define GIT_REVISION_SHORT          \"${GIT_REVISION_SHORT}\""
    VERSION_CONTENT "${VERSION_CONTENT}")

string(REGEX REPLACE
    "#define GIT_DIRTY[ \t]+\"[^\"]*\""
    "#define GIT_DIRTY                   \"${GIT_DIRTY}\""
    VERSION_CONTENT "${VERSION_CONTENT}")

string(REGEX REPLACE
    "#define GIT_VERSION[ \t]+\"[^\"]*\""
    "#define GIT_VERSION                 \"${GIT_VERSION}\""
    VERSION_CONTENT "${VERSION_CONTENT}")

# Strip Windows-only stuff that's meaningless on Android
if(ANDROID)
    string(REGEX REPLACE
        "#define VER_ORIGINAL_FILENAME_STR[^\n]*\n"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
    string(REGEX REPLACE
        "#define VER_INTERNAL_NAME_STR[^\n]*\n"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
    string(REGEX REPLACE
        "#ifdef _DEBUG[^#]*#define VER_VER_DEBUG[^#]*#else[^#]*#define VER_VER_DEBUG[^#]*#endif"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
    string(REGEX REPLACE
        "#define VER_FILEOS[^\n]*\n"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
    string(REGEX REPLACE
        "#define VER_FILEFLAGS[^\n]*\n"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
    string(REGEX REPLACE
        "#define VER_FILETYPE[^\n]*\n"
        ""
        VERSION_CONTENT "${VERSION_CONTENT}")
endif()

# Only write if content changed (avoids unnecessary rebuilds)
set(_write TRUE)
if(EXISTS "${VERSION_HEADER_OUT}")
    file(READ "${VERSION_HEADER_OUT}" _existing)
    if("${_existing}" STREQUAL "${VERSION_CONTENT}")
        set(_write FALSE)
    endif()
endif()

if(_write)
    file(WRITE "${VERSION_HEADER_OUT}" "${VERSION_CONTENT}")
    message(STATUS "Generated ${VERSION_HEADER_OUT}")
else()
    message(STATUS "version.h unchanged, skipping write")
endif()