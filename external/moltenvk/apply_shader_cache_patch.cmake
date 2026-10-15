find_program(_nxemu_git git REQUIRED)
set(_nxemu_source "${SOURCE_DIR}/MoltenVK")

function(set_patch patch enabled)
    execute_process(COMMAND "${_nxemu_git}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${_nxemu_source}"
        RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(enabled AND NOT applied EQUAL 0)
        execute_process(COMMAND "${_nxemu_git}" apply --check "${patch}"
            WORKING_DIRECTORY "${_nxemu_source}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${_nxemu_git}" apply "${patch}"
            WORKING_DIRECTORY "${_nxemu_source}" COMMAND_ERROR_IS_FATAL ANY)
    elseif(NOT enabled AND applied EQUAL 0)
        execute_process(COMMAND "${_nxemu_git}" apply --reverse "${patch}"
            WORKING_DIRECTORY "${_nxemu_source}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
endfunction()

# Revert only identified dependent patches when returning to the reference build.
# Never reset the checkout or discard unidentified local changes.
if(NOT ENABLE_SHADER_CACHE)
    set_patch("${BINARY_PATCH_FILE}" FALSE)
    set_patch("${PATCH_FILE}" FALSE)
endif()
file(GLOB base_patches "${SOURCE_DIR}/000*.patch")
foreach(patch IN LISTS base_patches)
    set_patch("${patch}" TRUE)
endforeach()
if(ENABLE_SHADER_CACHE)
    execute_process(COMMAND "${_nxemu_git}" apply --reverse --check "${BINARY_PATCH_FILE}"
        WORKING_DIRECTORY "${_nxemu_source}" RESULT_VARIABLE binary_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT binary_applied EQUAL 0)
        set_patch("${PATCH_FILE}" TRUE)
        set_patch("${BINARY_PATCH_FILE}" TRUE)
    endif()
endif()
