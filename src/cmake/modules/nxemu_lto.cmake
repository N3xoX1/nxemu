set(_nxemu_lto_default OFF)
if(APPLE OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_nxemu_lto_default ON)
endif()
option(NXEMU_ENABLE_LTO "Enable IPO in Release, RelWithDebInfo and MinSizeRel" ${_nxemu_lto_default})
option(NXEMU_ENABLE_THINLTO_CACHE "Cache incremental ThinLTO link results when supported" ON)
include(nxemu_android_ipo)
include(nxemu_linker)

if(NXEMU_ENABLE_LTO)
    set(_nxemu_lto_mode "LTO")
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$" AND
       "-flto=thin" IN_LIST CMAKE_CXX_COMPILE_OPTIONS_IPO)
        set(_nxemu_lto_mode "ThinLTO")
    endif()
    set(_nxemu_cache_option "")
    if(NXEMU_ENABLE_THINLTO_CACHE AND _nxemu_lto_mode STREQUAL "ThinLTO")
        set(NXEMU_THINLTO_CACHE_DIR "${CMAKE_BINARY_DIR}/thinlto-cache" CACHE PATH "ThinLTO cache directory")
        get_filename_component(_nxemu_cache_dir "${NXEMU_THINLTO_CACHE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
        foreach(_config Release RelWithDebInfo MinSizeRel)
            file(MAKE_DIRECTORY "${_nxemu_cache_dir}/${_config}")
        endforeach()
        if(APPLE)
            set(_nxemu_cache_option "LINKER:-cache_path_lto,${_nxemu_cache_dir}/$<CONFIG>")
        elseif(ANDROID OR NXEMU_SELECTED_LINKER STREQUAL "LLD" OR CMAKE_LINKER_TYPE STREQUAL "LLD")
            set(_nxemu_cache_option "LINKER:--thinlto-cache-dir=${_nxemu_cache_dir}/$<CONFIG>")
        endif()
    endif()

    # Check the actual compiler, archiver and linker, including shared modules.
    # Use the project signature for compatibility with CMake 3.22 (Android).
    function(_nxemu_check_ipo result output cache_option)
        set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
        set(_probe_flags
            "-DCMAKE_BUILD_TYPE:STRING=Release"
            "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
            "-DNXEMU_PROBE_LINKER_OPTION:STRING=${NXEMU_LINKER_OPTION}"
            "-DNXEMU_PROBE_CACHE_OPTION:STRING=${cache_option}")
        foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR
                     CMAKE_SYSROOT CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET
                     CMAKE_OBJCXX_COMPILER CMAKE_OBJCXX_COMPILER_TARGET
                     CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET
                     CMAKE_ANDROID_ARCH_ABI CMAKE_SYSTEM_VERSION CMAKE_ANDROID_NDK
                     CMAKE_ANDROID_NDK_VERSION ANDROID_ABI ANDROID_PLATFORM ANDROID_STL
                     CMAKE_LINKER_TYPE CMAKE_AR CMAKE_RANLIB
                     CMAKE_C_COMPILER_AR CMAKE_C_COMPILER_RANLIB
                     CMAKE_CXX_COMPILER_AR CMAKE_CXX_COMPILER_RANLIB
                     CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_OBJCXX_FLAGS
                     CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
                     CMAKE_POLICY_DEFAULT_CMP0141
                     CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE CMAKE_OBJCXX_FLAGS_RELEASE
                     CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                     CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_SHARED_LINKER_FLAGS_RELEASE
                     ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES})
            if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
                string(REPLACE ";" "\\;" _value "${${_var}}")
                list(APPEND _probe_flags "-D${_var}:STRING=${_value}")
            endif()
        endforeach()
        try_compile(_supported
            "${CMAKE_BINARY_DIR}/CMakeFiles/NxemuIPOProbe"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ipo_probe"
            NxemuIPOProbe ipo_probe
            CMAKE_FLAGS ${_probe_flags}
            OUTPUT_VARIABLE _output)
        set(${result} "${_supported}" PARENT_SCOPE)
        set(${output} "${_output}" PARENT_SCOPE)
    endfunction()

    _nxemu_check_ipo(_nxemu_ipo_supported _nxemu_ipo_output "")
    file(WRITE "${CMAKE_BINARY_DIR}/CMakeFiles/nxemu-ipo-check.log" "${_nxemu_ipo_output}")
    if(NOT _nxemu_ipo_supported)
        message(FATAL_ERROR "${_nxemu_lto_mode} compile/archive/link check failed. See CMakeFiles/nxemu-ipo-check.log. Set NXEMU_ENABLE_LTO=OFF to disable it.\n${_nxemu_ipo_output}")
    endif()
    if(_nxemu_cache_option)
        _nxemu_check_ipo(_nxemu_cache_supported _nxemu_cache_output "${_nxemu_cache_option}")
        file(WRITE "${CMAKE_BINARY_DIR}/CMakeFiles/nxemu-thinlto-cache-check.log" "${_nxemu_cache_output}")
        if(NOT _nxemu_cache_supported)
            message(WARNING "ThinLTO cache is unsupported by this linker; continuing without it. See CMakeFiles/nxemu-thinlto-cache-check.log.")
            set(_nxemu_cache_option "")
        endif()
    endif()
    # Some bundled dependencies declare older policy versions. They must not
    # silently ignore the IPO properties inherited from this project.
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
    if(APPLE AND CMAKE_CXX_COMPILE_OPTIONS_IPO)
        # CMake's IPO language support does not include Objective-C++. Compile
        # these sources to the same bitcode format; the C++ linker handles them.
        add_compile_options("$<$<AND:$<COMPILE_LANGUAGE:OBJCXX>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:${CMAKE_CXX_COMPILE_OPTIONS_IPO}>")
    endif()
    if(_nxemu_cache_option)
        add_link_options("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:${_nxemu_cache_option}>")
    endif()
    message(STATUS "${_nxemu_lto_mode}: Release/RelWithDebInfo/MinSizeRel; Debug unchanged")
endif()
unset(_nxemu_lto_default)
