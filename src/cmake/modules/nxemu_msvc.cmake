# Compiler and linker settings of the Windows (MSVC) build.
#
# Common defaults preserve user/toolchain flags. Historical stack-cookie
# settings are applied only to targets covered by the maintained VS projects.
# Only the binaries NxEmu ships (the
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
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# /MP       compile the files of a target in parallel
# /GF       pool identical strings
# /Gy       one section per function, so the linker can drop and fold them
# /Zi /FS   debug information in a PDB that parallel compilers can share
set(_nxemu_debug_flags "/Zi /FS")
if(POLICY CMP0141)
    if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    endif()
    set(_nxemu_debug_flags "/FS")
endif()
# Add a default only when no configuration supplies that option family.
# In particular, never replace CMake's or a toolchain's optimisation flags.
macro(_nxemu_default_flag lang flag family)
    set(_nxemu_user_flags "${CMAKE_${lang}_FLAGS}")
    set(_nxemu_configs Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES _nxemu_configs)
    foreach(_config IN LISTS _nxemu_configs)
        string(TOUPPER "${_config}" _config_upper)
        string(APPEND _nxemu_user_flags " ${CMAKE_${lang}_FLAGS_${_config_upper}}")
    endforeach()
    if(NOT _nxemu_user_flags MATCHES "(^|[ \t])[-/]${family}([ \t]|$)")
        string(APPEND CMAKE_${lang}_FLAGS " ${flag}")
    endif()
endmacro()
foreach(_nxemu_lang C CXX)
    _nxemu_default_flag(${_nxemu_lang} /W3 "(W[0-4]|Wall|w)")
    _nxemu_default_flag(${_nxemu_lang} /utf-8 "(utf-8|source-charset:[^ \t]+|execution-charset:[^ \t]+)")
    _nxemu_default_flag(${_nxemu_lang} /MP "MP[0-9]*")
    _nxemu_default_flag(${_nxemu_lang} /GF "GF-?")
    _nxemu_default_flag(${_nxemu_lang} /Gy "Gy-?")
    _nxemu_default_flag(${_nxemu_lang} /fp:precise "fp:[^ \t]+")
    if(_nxemu_debug_flags MATCHES "Zi")
        _nxemu_default_flag(${_nxemu_lang} /Zi "Z[iI7]")
    endif()
    _nxemu_default_flag(${_nxemu_lang} /FS "FS-?")
endforeach()
_nxemu_default_flag(CXX /EHsc "EH[^ \t]+")
_nxemu_default_flag(CXX /GR- "GR-?")
unset(_nxemu_lang)
unset(_nxemu_debug_flags)

function(nxemu_apply_legacy_stack_settings target)
    if(NOT MSVC OR NOT TARGET ${target})
        return()
    endif()
    get_target_property(_imported ${target} IMPORTED)
    if(_imported)
        return()
    endif()
    # Match platform.props, while respecting an explicit /GS or /GS- in
    # the developer's flags for each language/configuration.
    set(_configs Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES _configs)
    foreach(_lang C CXX)
        foreach(_config IN LISTS _configs)
            string(TOUPPER "${_config}" _upper)
            set(_flags "${CMAKE_${_lang}_FLAGS} ${CMAKE_${_lang}_FLAGS_${_upper}}")
            if(NOT _flags MATCHES "(^|[ \t])[-/]GS-?([ \t]|$)")
                target_compile_options(${target} PRIVATE
                    "$<$<AND:$<COMPILE_LANGUAGE:${_lang}>,$<CONFIG:${_config}>>:/GS->")
            endif()
        endforeach()
    endforeach()
endfunction()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    "$<$<CONFIG:Debug>:DEBUG>"
)
