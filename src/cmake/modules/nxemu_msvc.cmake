# Compiler and linker settings of the Windows (MSVC) build.
#
# Every target, bundled third-party libraries included, is built with the
# same code generation settings. Only the binaries NxEmu ships (the
# executable and its modules) get the linker settings, through
# nxemu_apply_link_settings().

# Libraries that set an older minimum CMake version still follow
# CMAKE_MSVC_RUNTIME_LIBRARY.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

function(nxemu_apply_link_settings target)
    if(NOT MSVC)
        return()
    endif()
    target_link_options(${target} PRIVATE
        /DEBUG
        /MAP
        /INCREMENTAL:NO
        /DYNAMICBASE:NO
        /OPT:REF
        /OPT:ICF=3
        # A delay-loaded library the target does not import is not an error.
        /IGNORE:4199
        /DELAYLOAD:advapi32.dll
        /DELAYLOAD:userenv.dll
        /DELAYLOAD:shell32.dll
        /DELAYLOAD:shlwapi.dll
        /DELAYLOAD:ws2_32.dll
        /DELAYLOAD:version.dll
        /DELAYLOAD:wtsapi32.dll
    )
    target_link_libraries(${target} PRIVATE delayimp)
endfunction()

# RmlUi relies on dynamic_cast; the directories that build or derive from it
# call this before declaring their targets.
macro(nxemu_enable_rtti)
    if(MSVC)
        string(REPLACE "/GR-" "/GR" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif()
endmacro()

if(NOT MSVC)
    return()
endif()

# Static C runtime: the executable and the modules need no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# /MP       compile the files of a target in parallel
# /GF       pool identical strings
# /GS-      no stack cookies
# /Gy       one section per function, so the linker can drop and fold them
# /Zi /FS   debug information in a PDB that parallel compilers can share
set(_nxemu_debug_flags "/Zi /FS")
if(POLICY CMP0141)
    if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    endif()
    set(_nxemu_debug_flags "/FS")
endif()
foreach(_nxemu_lang C CXX)
    set(CMAKE_${_nxemu_lang}_FLAGS "/DWIN32 /D_WINDOWS /W3 /utf-8 /MP /GF /GS- /Gy /fp:precise ${_nxemu_debug_flags}")
    set(CMAKE_${_nxemu_lang}_FLAGS_DEBUG "/Od /RTC1 /D_DEBUG /DDEBUG")
    # /O2 /Oi /Ot   optimise for speed, with intrinsics
    # /Oy-          keep frame pointers, for usable call stacks
    # /Gw           one section per global, so the linker can drop and fold them
    set(CMAKE_${_nxemu_lang}_FLAGS_RELEASE "/O2 /Oi /Ot /Oy- /Gw /DNDEBUG")
endforeach()
unset(_nxemu_lang)
unset(_nxemu_debug_flags)
string(APPEND CMAKE_CXX_FLAGS " /EHsc /GR-")

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)
