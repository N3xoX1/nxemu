if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED SRC OR NOT DEFINED DEST)
        message(FATAL_ERROR "nxemu_setup_environment.cmake -P requires -DSRC= and -DDEST=")
    endif()
    if(NOT EXISTS "${DEST}")
        get_filename_component(_dir "${DEST}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(COPY_FILE "${SRC}" "${DEST}")
    endif()
    return()
endif()

function(nxemu_setup_environment target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "nxemu_setup_environment: target '${target}' does not exist")
    endif()

    set(_src_config "${CMAKE_SOURCE_DIR}/config/NxEmu.config")
    set(_dev_config "${CMAKE_SOURCE_DIR}/config/NxEmu.config.development")
    if(NOT EXISTS "${_src_config}" AND EXISTS "${_dev_config}")
        file(COPY_FILE "${_dev_config}" "${_src_config}")
    endif()

    set(_config_src "${_src_config}")
    if(NOT EXISTS "${_config_src}")
        set(_config_src "${_dev_config}")
    endif()
    if(NOT EXISTS "${_config_src}")
        return()
    endif()

    set(_out "$<TARGET_FILE_DIR:${target}>")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DSRC=${_config_src}"
            "-DDEST=${_out}/user/config/NxEmu.config"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Installing user/config/NxEmu.config next to ${target}"
    )
endfunction()
