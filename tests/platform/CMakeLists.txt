# Native code execution on macOS arm64. Test executables declare SDK 12.3: below 13.0, XNU
# keeps x18 for the whole process, as in NxEmu. The headers and libraries stay those of the
# installed SDK.
set(NXEMU_TEST_LEGACY_SDK 12.3)
set(NXEMU_TEST_LEGACY_SDK_LINK "LINKER:-platform_version,macos,${CMAKE_OSX_DEPLOYMENT_TARGET},${NXEMU_TEST_LEGACY_SDK}")

# Ad-hoc signature without hardened runtime: the NCE maps guest code from shared memory.
function(nxemu_platform_executable name)
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_custom_command(TARGET ${name} POST_BUILD COMMAND codesign -s - -f $<TARGET_FILE:${name}> VERBATIM)
endfunction()

function(nxemu_platform_test name)
    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS platform TIMEOUT 600)
endfunction()

# NceDirectMap page model.
add_executable(test_direct_map nce_direct_map/test_direct_map.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_common/nce_direct_map.cpp")
target_include_directories(test_direct_map PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_direct_map PRIVATE FMT_HEADER_ONLY)
target_link_libraries(test_direct_map PRIVATE nxemu_test_support)
target_link_options(test_direct_map PRIVATE ${NXEMU_TEST_LEGACY_SDK_LINK})
nxemu_platform_executable(test_direct_map)
nxemu_platform_test(test_direct_map test_direct_map)

# Backend selection by address space width, and refusal when the host does not keep x18.
set(NCE_SETTINGS_SOURCES
    nce_address_space/test_settings.cpp
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/cpu_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/cpu_enum_strings.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/json.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/json_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/std_string.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/platform.cpp")
foreach(_name test_nce_settings test_nce_settings_current_sdk)
    add_executable(${_name} ${NCE_SETTINGS_SOURCES})
    target_include_directories(${_name} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/fmt/include")
    target_compile_definitions(${_name} PRIVATE FMT_HEADER_ONLY NXEMU_HAS_NCE)
    target_link_libraries(${_name} PRIVATE nxemu_test_support)
    nxemu_platform_executable(${_name})
endforeach()
target_link_options(test_nce_settings PRIVATE ${NXEMU_TEST_LEGACY_SDK_LINK})
nxemu_platform_test(test_nce_settings test_nce_settings)
nxemu_platform_test(test_nce_settings_current_sdk test_nce_settings_current_sdk --host-without-x18)

# Runtime against the real nxemu-cpu module: SVC, interrupts, NRO modules, atomics.
set(NCE_RUNTIME_INCLUDES "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/nxemu-cpu"
    "${CMAKE_SOURCE_DIR}/external/oaknut/include" "${CMAKE_SOURCE_DIR}/external/dynarmic/src"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
foreach(_name nce_runtime nce_runtime_modules)
    add_executable(${_name} nce_macos_runtime/${_name}.cpp)
    target_include_directories(${_name} PRIVATE ${NCE_RUNTIME_INCLUDES})
    target_link_libraries(${_name} PRIVATE nxemu-cpu)
    target_link_options(${_name} PRIVATE ${NXEMU_TEST_LEGACY_SDK_LINK})
    nxemu_platform_executable(${_name})
endforeach()
nxemu_platform_test(nce_runtime nce_runtime)
nxemu_platform_test(nce_runtime_interrupts nce_runtime interrupts)
nxemu_platform_test(nce_runtime_modules nce_runtime_modules)

# Native mapping of guest memory through Mach memory entries.
add_executable(nce_native_mapping nce_macos_runtime/native_mapping.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_common/nce_direct_map.cpp")
target_include_directories(nce_native_mapping PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/external/fmt/include")
add_dependencies(nce_native_mapping fmt)
target_link_libraries(nce_native_mapping PRIVATE $<TARGET_FILE:fmt>)
nxemu_platform_executable(nce_native_mapping)
nxemu_platform_test(nce_native_mapping nce_native_mapping)
