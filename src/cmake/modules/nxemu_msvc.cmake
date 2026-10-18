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
