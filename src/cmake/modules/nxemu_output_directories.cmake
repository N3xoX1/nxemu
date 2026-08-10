function(nxemu_set_app_output target)
    set(_out "${CMAKE_SOURCE_DIR}/bin/${NXEMU_PLATFORM_DIR}/$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_out}"
        LIBRARY_OUTPUT_DIRECTORY "${_out}"
        ARCHIVE_OUTPUT_DIRECTORY "${_out}/lib"
    )
endfunction()

function(nxemu_set_module_output target module_name)
    set(_out "${CMAKE_SOURCE_DIR}/modules/${NXEMU_PLATFORM_DIR}/${module_name}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_out}"
        LIBRARY_OUTPUT_DIRECTORY "${_out}"
        ARCHIVE_OUTPUT_DIRECTORY "${_out}"
    )
endfunction()
