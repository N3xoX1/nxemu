# Native code execution on macOS arm64. Test executables declare SDK 12.3: below 13.0, XNU
# keeps x18 for the whole process, as in NxEmu. The headers and libraries stay those of the
# installed SDK.
enable_language(C ASM)
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

# Validation suite: memory, ARM64 and transitions with the production patcher.
set(NCE_MACOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/nce_macos")
add_library(nce_macos_objects OBJECT
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/nce/patcher.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/nce/decode.cpp"
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/arm_dynarmic.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_common/arm64/native_clock.cpp"
    "${CMAKE_SOURCE_DIR}/external/mcl/src/assert.cpp"
    "${NCE_MACOS_DIR}/src/child.cpp"
    "${NCE_MACOS_DIR}/src/main.cpp"
    "${NCE_MACOS_DIR}/src/nce_runtime.cpp"
    "${NCE_MACOS_DIR}/src/probes.cpp"
    "${NCE_MACOS_DIR}/src/report.cpp"
    "${NCE_MACOS_DIR}/src/test_arm64.cpp"
    "${NCE_MACOS_DIR}/src/test_memory.cpp"
    "${NCE_MACOS_DIR}/src/test_read_thunks.cpp"
    "${NCE_MACOS_DIR}/src/test_store_thunks.cpp"
    "${NCE_MACOS_DIR}/src/test_transitions.cpp"
    "${NCE_MACOS_DIR}/src/metal_smoke.mm"
    "${NCE_MACOS_DIR}/src/nce_darwin.S"
    "${NCE_MACOS_DIR}/src/arm64_probes.S"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/support_stubs.cpp")
target_include_directories(nce_macos_objects PRIVATE
    "${NCE_MACOS_DIR}/shim" "${NCE_MACOS_DIR}/src" "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-cpu" "${CMAKE_SOURCE_DIR}/external/oaknut/include"
    "${CMAKE_SOURCE_DIR}/external/fmt/include" "${CMAKE_SOURCE_DIR}/external/mcl/include")
target_compile_definitions(nce_macos_objects PRIVATE FMT_HEADER_ONLY NCEV_LEGACY_SDK="${NXEMU_TEST_LEGACY_SDK}")
target_compile_features(nce_macos_objects PRIVATE cxx_std_20)
target_compile_options(nce_macos_objects PRIVATE $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wno-unused-function>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nxemu-cpu/nce/patcher.cpp" TARGET_DIRECTORY nce_macos_objects
    PROPERTIES COMPILE_OPTIONS -Wno-unused-variable)

add_executable(nce_macos_validate $<TARGET_OBJECTS:nce_macos_objects>)
target_link_libraries(nce_macos_validate PRIVATE "-framework Metal" "-framework Foundation")
target_link_options(nce_macos_validate PRIVATE ${NXEMU_TEST_LEGACY_SDK_LINK})
nxemu_platform_executable(nce_macos_validate)
# Control for A3.2: the same program declaring the installed SDK.
add_executable(nce_macos_validate_current_sdk $<TARGET_OBJECTS:nce_macos_objects>)
target_link_libraries(nce_macos_validate_current_sdk PRIVATE "-framework Metal" "-framework Foundation")
nxemu_platform_executable(nce_macos_validate_current_sdk)
# Step 2: an executable with a 16 KiB __PAGEZERO.
add_executable(pagezero_probe "${NCE_MACOS_DIR}/src/pagezero_probe.c")
target_link_options(pagezero_probe PRIVATE LINKER:-pagezero_size,0x4000)
nxemu_platform_executable(pagezero_probe)
add_dependencies(nce_macos_validate nce_macos_validate_current_sdk pagezero_probe)
nxemu_platform_test(nce_macos_validate nce_macos_validate)

# NceDirectMap page model.
add_executable(test_direct_map "${NCE_MACOS_DIR}/direct_map/test_direct_map.cpp"
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
