function(nxemu_set_app_output target)
    set(_out "${CMAKE_SOURCE_DIR}/bin/${NXEMU_PLATFORM_DIR}/$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_out}"
        LIBRARY_OUTPUT_DIRECTORY "${_out}"
        ARCHIVE_OUTPUT_DIRECTORY "${_out}/lib"
    )
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE")
        nxemu_apply_link_settings(${target})
    endif()
endfunction()

function(nxemu_set_module_output target module_name)
    if(APPLE)
        # Each build tree/configuration owns its modules. Packaging copies the
        # actual target files, rather than picking up another build's binaries.
        set(_out "${CMAKE_BINARY_DIR}/modules/${NXEMU_PLATFORM_DIR}/$<CONFIG>/${module_name}")
    else()
        set(_out "${CMAKE_SOURCE_DIR}/modules/${NXEMU_PLATFORM_DIR}/${module_name}")
        if(NOT ANDROID)
            set_target_properties(${target} PROPERTIES DEBUG_POSTFIX "_d")
        endif()
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_out}"
        LIBRARY_OUTPUT_DIRECTORY "${_out}"
        ARCHIVE_OUTPUT_DIRECTORY "${_out}"
    )
    nxemu_apply_link_settings(${target})
endfunction()
