cmake_minimum_required(VERSION 3.18)
project(nxemu-cpu LANGUAGES C CXX ASM)
 
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VERSION_HEADER_IN "${CMAKE_SOURCE_DIR}/src/nxemu-cpu/version.h.in")
set(VERSION_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/nxemu-cpu/version.h")
include(generate_version_header)

# Compiler flags
set(DYNARMIC_CXX_FLAGS
   -Wall
   -Wextra
   -Wcast-qual
   -pedantic
   -Wno-missing-braces)

if (CXX_GCC)
   # GCC produces bogus -Warray-bounds warnings from xbyak headers for code paths that are not
   # actually reachable.  Specifically, it happens in cases where some code casts an Operand&
   # to Address& after first checking isMEM(), and that code is inlined in a situation where
   # GCC knows that the variable is actually a Reg64.  isMEM() will never return true for a
   # Reg64, but GCC doesn't know that.
   list(APPEND DYNARMIC_CXX_FLAGS -Wno-array-bounds)
   list(APPEND DYNARMIC_CXX_FLAGS -Wstack-usage=4096)
endif()

if (CXX_CLANG)
   # Bracket depth determines maximum size of a fold expression in Clang since 9c9974c3ccb6.
   # And this in turns limits the size of a std::array.
   list(APPEND DYNARMIC_CXX_FLAGS -fbracket-depth=1024)
   # Clang mistakenly blames CMake for using unused arguments during compilation
   list(APPEND DYNARMIC_CXX_FLAGS -Wno-unused-command-line-argument)
endif()

add_library(nxemu-cpu SHARED
    arm_dynarmic.cpp
    arm_dynarmic.h
    arm_dynarmic_32.cpp
    arm_dynarmic_64.cpp
    arm_dynarmic_64.h
    cpu_enum_strings.cpp
    cpu_enum_strings.h
    cpu_manager.cpp
    cpu_manager.h
    cpu_settings.cpp
    cpu_settings.h
    cpu_settings_identifiers.h
    dynarmic_cp15.cpp
    dynarmic_cp15.h
    exclusive_monitor_interface.cpp
    nxemu-cpu.cpp
    arm_dynarmic_32.h
    exclusive_monitor_interface.h
    dynarmic/backend/block_range_information.cpp
    dynarmic/backend/block_range_information.h
    dynarmic/backend/exception_handler.h
    dynarmic/common/always_false.h
    dynarmic/common/cast_util.h
    dynarmic/common/crypto/aes.cpp
    dynarmic/common/crypto/aes.h
    dynarmic/common/crypto/crc32.cpp
    dynarmic/common/crypto/crc32.h
    dynarmic/common/crypto/sm4.cpp
    dynarmic/common/crypto/sm4.h
    dynarmic/common/fp/fpcr.h
    dynarmic/common/fp/fpsr.h
    dynarmic/common/fp/fused.cpp
    dynarmic/common/fp/fused.h
    dynarmic/common/fp/info.h
    dynarmic/common/fp/mantissa_util.h
    dynarmic/common/fp/op.h
    dynarmic/common/fp/op/FPCompare.cpp
    dynarmic/common/fp/op/FPCompare.h
    dynarmic/common/fp/op/FPConvert.cpp
    dynarmic/common/fp/op/FPConvert.h
    dynarmic/common/fp/op/FPMulAdd.cpp
    dynarmic/common/fp/op/FPMulAdd.h
    dynarmic/common/fp/op/FPNeg.h
    dynarmic/common/fp/op/FPRecipEstimate.cpp
    dynarmic/common/fp/op/FPRecipEstimate.h
    dynarmic/common/fp/op/FPRecipExponent.cpp
    dynarmic/common/fp/op/FPRecipExponent.h
    dynarmic/common/fp/op/FPRecipStepFused.cpp
    dynarmic/common/fp/op/FPRecipStepFused.h
    dynarmic/common/fp/op/FPRoundInt.cpp
    dynarmic/common/fp/op/FPRoundInt.h
    dynarmic/common/fp/op/FPRSqrtEstimate.cpp
    dynarmic/common/fp/op/FPRSqrtEstimate.h
    dynarmic/common/fp/op/FPRSqrtStepFused.cpp
    dynarmic/common/fp/op/FPRSqrtStepFused.h
    dynarmic/common/fp/op/FPToFixed.cpp
    dynarmic/common/fp/op/FPToFixed.h
    dynarmic/common/fp/process_exception.cpp
    dynarmic/common/fp/process_exception.h
    dynarmic/common/fp/process_nan.cpp
    dynarmic/common/fp/process_nan.h
    dynarmic/common/fp/rounding_mode.h
    dynarmic/common/fp/unpacked.cpp
    dynarmic/common/fp/unpacked.h
    dynarmic/common/fp/util.h
    dynarmic/common/llvm_disassemble.cpp
    dynarmic/common/llvm_disassemble.h
    dynarmic/common/lut_from_list.h
    dynarmic/common/math_util.cpp
    dynarmic/common/math_util.h
    dynarmic/common/memory_pool.cpp
    dynarmic/common/memory_pool.h
    dynarmic/common/safe_ops.h
    dynarmic/common/spin_lock.h
    dynarmic/common/string_util.h
    dynarmic/common/u128.cpp
    dynarmic/common/u128.h
    dynarmic/common/variant_util.h
    dynarmic/frontend/A32/a32_types.cpp
    dynarmic/frontend/A32/a32_types.h
    dynarmic/frontend/A64/a64_types.cpp
    dynarmic/frontend/A64/a64_types.h
    dynarmic/frontend/decoder/decoder_detail.h
    dynarmic/frontend/decoder/matcher.h
    dynarmic/frontend/imm.cpp
    dynarmic/frontend/imm.h
    dynarmic/interface/optimization_flags.h
    dynarmic/ir/acc_type.h
    dynarmic/ir/basic_block.cpp
    dynarmic/ir/basic_block.h
    dynarmic/ir/cond.h
    dynarmic/ir/ir_emitter.cpp
    dynarmic/ir/ir_emitter.h
    dynarmic/ir/location_descriptor.cpp
    dynarmic/ir/location_descriptor.h
    dynarmic/ir/microinstruction.cpp
    dynarmic/ir/microinstruction.h
    dynarmic/ir/opcodes.cpp
    dynarmic/ir/opcodes.h
    dynarmic/ir/opcodes.inc
    dynarmic/ir/opt/constant_propagation_pass.cpp
    dynarmic/ir/opt/dead_code_elimination_pass.cpp
    dynarmic/ir/opt/identity_removal_pass.cpp
    dynarmic/ir/opt/ir_matcher.h
    dynarmic/ir/opt/naming_pass.cpp
    dynarmic/ir/opt/passes.h
    dynarmic/ir/opt/polyfill_pass.cpp
    dynarmic/ir/opt/verification_pass.cpp
    dynarmic/ir/terminal.h
    dynarmic/ir/type.cpp
    dynarmic/ir/type.h
    dynarmic/ir/value.cpp
    dynarmic/ir/value.h
    dynarmic/frontend/A32/a32_ir_emitter.cpp
    dynarmic/frontend/A32/a32_ir_emitter.h
    dynarmic/frontend/A32/a32_location_descriptor.cpp
    dynarmic/frontend/A32/a32_location_descriptor.h
    dynarmic/frontend/A32/a32_types.cpp
    dynarmic/frontend/A32/decoder/arm.h
    dynarmic/frontend/A32/decoder/arm.inc
    dynarmic/frontend/A32/decoder/asimd.h
    dynarmic/frontend/A32/decoder/asimd.inc
    dynarmic/frontend/A32/decoder/thumb16.h
    dynarmic/frontend/A32/decoder/thumb16.inc
    dynarmic/frontend/A32/decoder/thumb32.h
    dynarmic/frontend/A32/decoder/thumb32.inc
    dynarmic/frontend/A32/decoder/vfp.h
    dynarmic/frontend/A32/decoder/vfp.inc
    dynarmic/frontend/A32/disassembler/disassembler.h
    dynarmic/frontend/A32/FPSCR.h
    dynarmic/frontend/A32/ITState.h
    dynarmic/frontend/A32/PSR.h
    dynarmic/frontend/A32/translate/a32_translate.cpp
    dynarmic/frontend/A32/translate/a32_translate.h
    dynarmic/frontend/A32/translate/conditional_state.cpp
    dynarmic/frontend/A32/translate/conditional_state.h
    dynarmic/frontend/A32/translate/impl/a32_branch.cpp
    dynarmic/frontend/A32/translate/impl/a32_crc32.cpp
    dynarmic/frontend/A32/translate/impl/a32_exception_generating.cpp
    dynarmic/frontend/A32/translate/impl/a32_translate_impl.cpp
    dynarmic/frontend/A32/translate/impl/a32_translate_impl.h
    dynarmic/frontend/A32/translate/impl/asimd_load_store_structures.cpp
    dynarmic/frontend/A32/translate/impl/asimd_misc.cpp
    dynarmic/frontend/A32/translate/impl/asimd_one_reg_modified_immediate.cpp
    dynarmic/frontend/A32/translate/impl/asimd_three_regs.cpp
    dynarmic/frontend/A32/translate/impl/asimd_two_regs_misc.cpp
    dynarmic/frontend/A32/translate/impl/asimd_two_regs_scalar.cpp
    dynarmic/frontend/A32/translate/impl/asimd_two_regs_shift.cpp
    dynarmic/frontend/A32/translate/impl/barrier.cpp
    dynarmic/frontend/A32/translate/impl/coprocessor.cpp
    dynarmic/frontend/A32/translate/impl/data_processing.cpp
    dynarmic/frontend/A32/translate/impl/divide.cpp
    dynarmic/frontend/A32/translate/impl/extension.cpp
    dynarmic/frontend/A32/translate/impl/hint.cpp
    dynarmic/frontend/A32/translate/impl/load_store.cpp
    dynarmic/frontend/A32/translate/impl/misc.cpp
    dynarmic/frontend/A32/translate/impl/multiply.cpp
    dynarmic/frontend/A32/translate/impl/packing.cpp
    dynarmic/frontend/A32/translate/impl/parallel.cpp
    dynarmic/frontend/A32/translate/impl/reversal.cpp
    dynarmic/frontend/A32/translate/impl/saturated.cpp
    dynarmic/frontend/A32/translate/impl/status_register_access.cpp
    dynarmic/frontend/A32/translate/impl/synchronization.cpp
    dynarmic/frontend/A32/translate/impl/thumb16.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_branch.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_control.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_coprocessor.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_data_processing_modified_immediate.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_data_processing_plain_binary_immediate.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_data_processing_register.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_data_processing_shifted_register.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_load_byte.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_load_halfword.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_load_store_dual.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_load_store_multiple.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_load_word.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_long_multiply.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_misc.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_multiply.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_parallel.cpp
    dynarmic/frontend/A32/translate/impl/thumb32_store_single_data_item.cpp
    dynarmic/frontend/A32/translate/impl/vfp.cpp
    dynarmic/frontend/A32/translate/translate_arm.cpp
    dynarmic/frontend/A32/translate/translate_thumb.cpp
    dynarmic/interface/A32/a32.h
    dynarmic/interface/A32/arch_version.h
    dynarmic/interface/A32/config.h
    dynarmic/interface/A32/coprocessor.h
    dynarmic/interface/A32/coprocessor_util.h
    dynarmic/interface/A32/disassembler.h
    dynarmic/frontend/A64/a64_ir_emitter.cpp
    dynarmic/frontend/A64/a64_ir_emitter.h
    dynarmic/frontend/A64/a64_location_descriptor.cpp
    dynarmic/frontend/A64/a64_location_descriptor.h
    dynarmic/frontend/A64/decoder/a64.h
    dynarmic/frontend/A64/decoder/a64.inc
    dynarmic/frontend/A64/a64_types.cpp
    dynarmic/frontend/A64/translate/a64_translate.cpp
    dynarmic/frontend/A64/translate/a64_translate.h
    dynarmic/frontend/A64/translate/impl/a64_branch.cpp
    dynarmic/frontend/A64/translate/impl/a64_exception_generating.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_addsub.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_bitfield.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_conditional_compare.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_conditional_select.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_crc32.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_logical.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_multiply.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_pcrel.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_register.cpp
    dynarmic/frontend/A64/translate/impl/data_processing_shift.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_compare.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_conditional_compare.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_conditional_select.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_conversion_fixed_point.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_conversion_integer.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_data_processing_one_register.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_data_processing_three_register.cpp
    dynarmic/frontend/A64/translate/impl/floating_point_data_processing_two_register.cpp
    dynarmic/frontend/A64/translate/impl/impl.cpp
    dynarmic/frontend/A64/translate/impl/load_store_exclusive.cpp
    dynarmic/frontend/A64/translate/impl/load_store_load_literal.cpp
    dynarmic/frontend/A64/translate/impl/load_store_multiple_structures.cpp
    dynarmic/frontend/A64/translate/impl/load_store_no_allocate_pair.cpp
    dynarmic/frontend/A64/translate/impl/load_store_register_immediate.cpp
    dynarmic/frontend/A64/translate/impl/load_store_register_pair.cpp
    dynarmic/frontend/A64/translate/impl/load_store_register_register_offset.cpp
    dynarmic/frontend/A64/translate/impl/load_store_register_unprivileged.cpp
    dynarmic/frontend/A64/translate/impl/load_store_single_structure.cpp
    dynarmic/frontend/A64/translate/impl/move_wide.cpp
    dynarmic/frontend/A64/translate/impl/simd_across_lanes.cpp
    dynarmic/frontend/A64/translate/impl/simd_aes.cpp
    dynarmic/frontend/A64/translate/impl/simd_copy.cpp
    dynarmic/frontend/A64/translate/impl/simd_crypto_four_register.cpp
    dynarmic/frontend/A64/translate/impl/simd_crypto_three_register.cpp
    dynarmic/frontend/A64/translate/impl/simd_extract.cpp
    dynarmic/frontend/A64/translate/impl/simd_modified_immediate.cpp
    dynarmic/frontend/A64/translate/impl/simd_permute.cpp
    dynarmic/frontend/A64/translate/impl/simd_scalar_pairwise.cpp
    dynarmic/frontend/A64/translate/impl/simd_scalar_shift_by_immediate.cpp
    dynarmic/frontend/A64/translate/impl/simd_scalar_three_same.cpp
    dynarmic/frontend/A64/translate/impl/simd_scalar_two_register_misc.cpp
    dynarmic/frontend/A64/translate/impl/simd_scalar_x_indexed_element.cpp
    dynarmic/frontend/A64/translate/impl/simd_sha.cpp
    dynarmic/frontend/A64/translate/impl/simd_sha512.cpp
    dynarmic/frontend/A64/translate/impl/simd_shift_by_immediate.cpp
    dynarmic/frontend/A64/translate/impl/simd_table_lookup.cpp
    dynarmic/frontend/A64/translate/impl/simd_three_different.cpp
    dynarmic/frontend/A64/translate/impl/simd_three_same.cpp
    dynarmic/frontend/A64/translate/impl/simd_three_same_extra.cpp
    dynarmic/frontend/A64/translate/impl/simd_two_register_misc.cpp
    dynarmic/frontend/A64/translate/impl/simd_vector_x_indexed_element.cpp
    dynarmic/frontend/A64/translate/impl/system.cpp
    dynarmic/frontend/A64/translate/impl/system_flag_format.cpp
    dynarmic/frontend/A64/translate/impl/system_flag_manipulation.cpp
    dynarmic/frontend/A64/translate/impl/sys_dc.cpp
    dynarmic/frontend/A64/translate/impl/sys_ic.cpp
    dynarmic/frontend/imm.cpp
    dynarmic/ir/basic_block.cpp
    dynarmic/ir/ir_emitter.cpp
    dynarmic/ir/location_descriptor.cpp
    dynarmic/ir/microinstruction.cpp
    dynarmic/ir/opcodes.cpp
    dynarmic/ir/opt/a32_constant_memory_reads_pass.cpp
    dynarmic/ir/opt/a32_get_set_elimination_pass.cpp
    dynarmic/ir/opt/a64_callback_config_pass.cpp
    dynarmic/ir/opt/a64_get_set_elimination_pass.cpp
    dynarmic/ir/opt/a64_merge_interpret_blocks.cpp
    dynarmic/ir/opt/constant_propagation_pass.cpp
    dynarmic/ir/opt/dead_code_elimination_pass.cpp
    dynarmic/ir/opt/identity_removal_pass.cpp
    dynarmic/ir/opt/naming_pass.cpp
    dynarmic/ir/opt/polyfill_pass.cpp
    dynarmic/ir/opt/verification_pass.cpp
    dynarmic/ir/type.cpp
    dynarmic/ir/value.cpp
)

if(ARCHITECTURE_x86_64)
    target_compile_definitions(nxemu-cpu PRIVATE
        ARCHITECTURE_x86_64=1
        XBYAK_STRICT_CHECK_MEM_REG_SIZE=0
        XBYAK_OLD_DISP_CHECK=1
    )
 
    target_sources(nxemu-cpu PRIVATE
        dynarmic/common/spin_lock_x64.cpp
        dynarmic/common/x64_disassemble.cpp
        dynarmic/backend/x64/a32_emit_x64.cpp
        dynarmic/backend/x64/a32_emit_x64_memory.cpp
        dynarmic/backend/x64/a32_interface.cpp
        dynarmic/backend/x64/a32_jitstate.cpp
        dynarmic/backend/x64/a64_emit_x64.cpp
        dynarmic/backend/x64/a64_emit_x64_memory.cpp
        dynarmic/backend/x64/a64_interface.cpp
        dynarmic/backend/x64/a64_jitstate.cpp
        dynarmic/backend/x64/abi.cpp
        dynarmic/backend/x64/block_of_code.cpp
        dynarmic/backend/x64/callback.cpp
        dynarmic/backend/x64/constant_pool.cpp
        dynarmic/backend/x64/emit_x64.cpp
        dynarmic/backend/x64/emit_x64_aes.cpp
        dynarmic/backend/x64/emit_x64_crc32.cpp
        dynarmic/backend/x64/emit_x64_data_processing.cpp
        dynarmic/backend/x64/emit_x64_floating_point.cpp
        backend/x64/emit_x64_memory.cpp.inc
        backend/x64/emit_x64_memory.h
        dynarmic/backend/x64/emit_x64_packed.cpp
        dynarmic/backend/x64/emit_x64_saturation.cpp
        dynarmic/backend/x64/emit_x64_sha.cpp
        dynarmic/backend/x64/emit_x64_sm4.cpp
        dynarmic/backend/x64/emit_x64_vector.cpp
        dynarmic/backend/x64/emit_x64_vector_floating_point.cpp
        dynarmic/backend/x64/emit_x64_vector_saturation.cpp
        dynarmic/backend/x64/exclusive_monitor.cpp
        backend/x64/exclusive_monitor_friend.h
        backend/x64/host_feature.h
        dynarmic/backend/x64/hostloc.cpp
        backend/x64/hostloc.h
        backend/x64/jitstate_info.h
        backend/x64/oparg.h
        dynarmic/backend/x64/perf_map.cpp
        backend/x64/perf_map.h
        dynarmic/backend/x64/reg_alloc.cpp
        backend/x64/reg_alloc.h
        backend/x64/stack_layout.h
        dynarmic/backend/x64/verbose_debugging_output.cpp
        backend/x64/verbose_debugging_output.h
    )
 
    target_link_libraries(nxemu-cpu PRIVATE zydis)
endif()

if(ARCHITECTURE_arm64)
    target_compile_definitions(nxemu-cpu PRIVATE ARCHITECTURE_Aarch64=1)
 
    target_sources(nxemu-cpu PRIVATE
        dynarmic/backend/exception_handler_generic.cpp
        dynarmic/common/spin_lock_arm64.cpp
        dynarmic/backend/arm64/a32_jitstate.cpp
        dynarmic/backend/arm64/a32_address_space.cpp
        dynarmic/backend/arm64/a32_interface.cpp
        dynarmic/backend/arm64/abi.h
        dynarmic/backend/arm64/a64_address_space.cpp
        dynarmic/backend/arm64/a64_interface.cpp
        dynarmic/backend/arm64/abi.cpp
        dynarmic/backend/arm64/address_space.cpp
        dynarmic/backend/arm64/emit_arm64.cpp
        dynarmic/backend/arm64/emit_arm64_a32.cpp
        dynarmic/backend/arm64/emit_arm64_a32_coprocessor.cpp
        dynarmic/backend/arm64/emit_arm64_a32_memory.cpp
        dynarmic/backend/arm64/emit_arm64_a64.cpp
        dynarmic/backend/arm64/emit_arm64_a64_memory.cpp
        dynarmic/backend/arm64/emit_arm64_cryptography.cpp
        dynarmic/backend/arm64/emit_arm64_data_processing.cpp
        dynarmic/backend/arm64/emit_arm64_floating_point.cpp
        dynarmic/backend/arm64/emit_arm64_memory.cpp
        dynarmic/backend/arm64/emit_arm64_memory.h
        dynarmic/backend/arm64/emit_arm64_packed.cpp
        dynarmic/backend/arm64/emit_arm64_saturation.cpp
        dynarmic/backend/arm64/emit_arm64_vector.cpp
        dynarmic/backend/arm64/emit_arm64_vector_floating_point.cpp
        dynarmic/backend/arm64/emit_arm64_vector_saturation.cpp
        dynarmic/backend/arm64/emit_context.h
        dynarmic/backend/arm64/exclusive_monitor.cpp
        dynarmic/backend/arm64/fastmem.h
        dynarmic/backend/arm64/fpsr_manager.cpp
        dynarmic/backend/arm64/fpsr_manager.h
        dynarmic/backend/arm64/reg_alloc.cpp
        dynarmic/backend/arm64/reg_alloc.h
        dynarmic/backend/arm64/stack_layout.h
        dynarmic/backend/arm64/verbose_debugging_output.cpp
        dynarmic/backend/arm64/verbose_debugging_output.h
    )
 
    if(TARGET merry::oaknut)
        target_link_libraries(nxemu-cpu PRIVATE merry::oaknut)
    elseif(TARGET oaknut)
        target_link_libraries(nxemu-cpu PRIVATE oaknut)
    endif()
endif()

target_include_directories(nxemu-cpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/3rd_party/SoftFloat-3e/source/include
    ${CMAKE_SOURCE_DIR}/external/boost
    ${CMAKE_SOURCE_DIR}/external/mcl/include
    ${CMAKE_SOURCE_DIR}/external/fmt/include
    ${CMAKE_SOURCE_DIR}/external/robin-map/include
    ${CMAKE_SOURCE_DIR}/external/oaknut/include
    ${CMAKE_SOURCE_DIR}/external/zycore/include
    ${CMAKE_SOURCE_DIR}/external/zydis/include
    ${CMAKE_SOURCE_DIR}/external/xbyak
)

target_link_libraries(nxemu-cpu PRIVATE
    common
    fmt
    merry::mcl
    yuzu_common
)

if(ANDROID)
    target_link_libraries(nxemu-cpu PRIVATE log)
endif()
