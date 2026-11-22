// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/macro/macro_interpreter.h"

namespace Tegra {
namespace {
class MacroInterpreterImpl final : public CachedMacro {
public:
    explicit MacroInterpreterImpl(Engines::Maxwell3D& maxwell3d_, const std::vector<u32>& code_)
        : maxwell3d{maxwell3d_}, code{code_} {}

    void Execute(const std::vector<u32>& params, u32 method) override;

private:
    /// Resets the execution engine state, zeroing registers, etc.
    void Reset();

    /**
     * Executes a single macro instruction located at the current program counter. Returns whether
     * the interpreter should keep running.
     *
     * @param is_delay_slot Whether the current step is being executed due to a delay slot in a
     *                      previous instruction.
     */
    bool Step(bool is_delay_slot);

    /// Calculates the result of an ALU operation. src_a OP src_b;
    u32 GetALUResult(Macro::ALUOperation operation, u32 src_a, u32 src_b);

    /// Performs the result operation on the input result and stores it in the specified register
    /// (if necessary).
    void ProcessResult(Macro::ResultOperation operation, u32 reg, u32 result);

    /// Evaluates the branch condition and returns whether the branch should be taken or not.
    bool EvaluateBranchCondition(Macro::BranchCondition cond, u32 value) const;

    /// Reads an opcode at the current program counter location.
    Macro::Opcode GetOpcode() const;

    /// Returns the specified register's value. Register 0 is hardcoded to always return 0.
    u32 GetRegister(u32 register_id) const;

    /// Sets the register to the input value.
    void SetRegister(u32 register_id, u32 value);

    /// Sets the method address to use for the next Send instruction.
    void SetMethodAddress(u32 address);

    /// Calls a GPU Engine method with the input parameter.
    void Send(u32 value);

    /// Reads a GPU register located at the method address.
    u32 Read(u32 method) const;

    /// Returns the next parameter in the parameter queue.
    u32 FetchParameter();

    Engines::Maxwell3D& maxwell3d;

    /// Current program counter
    u32 pc{};
    /// Program counter to execute at after the delay slot is executed.
    std::optional<u32> delayed_pc;

    /// General purpose macro registers.
    std::array<u32, Macro::NUM_MACRO_REGISTERS> registers = {};

    /// Method address to use for the next Send instruction.
    Macro::MethodAddress method_address = {};

    /// The caller owns the parameters throughout Execute. A snapshot is only needed if the macro
    /// itself invokes another macro, which may mutate the caller's parameter vector.
    const u32* parameters = nullptr;
    std::vector<u32> stable_parameters;
    bool parameters_snapshotted = false;
    std::size_t num_parameters = 0;
    /// Index of the next parameter that will be fetched by the 'parm' instruction.
    u32 next_parameter_index = 0;

    bool carry_flag = false;
    const std::vector<u32>& code;
};

void MacroInterpreterImpl::Execute(const std::vector<u32>& params, u32 method) {
    Reset();

    if (params.empty()) [[unlikely]] {
        ASSERT_MSG(false, "MME macro received an empty parameter FIFO");
        return;
    }
    registers[1] = params[0];
    num_parameters = params.size();
    parameters = params.data();

    // Execute the code until we hit an exit condition.
    bool keep_executing = true;
    while (keep_executing) {
        keep_executing = Step(false);
    }

    // Assert the the macro used all the input parameters
    if (next_parameter_index != num_parameters) [[unlikely]] {
        ASSERT_MSG(false, "MME macro did not consume its parameter FIFO");
    }
}

void MacroInterpreterImpl::Reset() {
    registers = {};
    pc = 0;
    delayed_pc = {};
    method_address.raw = 0;
    parameters = nullptr;
    parameters_snapshotted = false;
    num_parameters = 0;
    // The next parameter index starts at 1, because $r1 already has the value of the first
    // parameter.
    next_parameter_index = 1;
    carry_flag = false;
}

bool MacroInterpreterImpl::Step(bool is_delay_slot) {
    u32 base_address = pc;

    Macro::Opcode opcode = GetOpcode();
    pc += 4;

    // Update the program counter if we were delayed
    if (delayed_pc) {
        ASSERT(is_delay_slot);
        pc = *delayed_pc;
        delayed_pc = {};
    }

    switch (opcode.operation) {
    case Macro::Operation::ALU: {
        u32 result = GetALUResult(opcode.alu_operation, GetRegister(opcode.src_a),
                                  GetRegister(opcode.src_b));
        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::AddImmediate: {
        ProcessResult(opcode.result_operation, opcode.dst,
                      GetRegister(opcode.src_a) + opcode.immediate);
        break;
    }
    case Macro::Operation::ExtractInsert: {
        u32 dst = GetRegister(opcode.src_a);
        u32 src = GetRegister(opcode.src_b);

        src = (src >> opcode.bf_src_bit) & opcode.GetBitfieldMask();
        dst &= ~(opcode.GetBitfieldMask() << opcode.bf_dst_bit);
        dst |= src << opcode.bf_dst_bit;
        ProcessResult(opcode.result_operation, opcode.dst, dst);
        break;
    }
    case Macro::Operation::ExtractShiftLeftImmediate: {
        // Maxwell's variable shifts follow the x86 macro JIT's 5-bit shift-count semantics.
        u32 dst = Macro::MaskShiftCount(GetRegister(opcode.src_a));
        u32 src = GetRegister(opcode.src_b);

        u32 result = ((src >> dst) & opcode.GetBitfieldMask()) << opcode.bf_dst_bit;

        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::ExtractShiftLeftRegister: {
        u32 dst = Macro::MaskShiftCount(GetRegister(opcode.src_a));
        u32 src = GetRegister(opcode.src_b);

        u32 result = ((src >> opcode.bf_src_bit) & opcode.GetBitfieldMask()) << dst;

        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::Read: {
        u32 result = Read(GetRegister(opcode.src_a) + opcode.immediate);
        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::Branch: {
        ASSERT_MSG(!is_delay_slot, "Executing a branch in a delay slot is not valid");
        u32 value = GetRegister(opcode.src_a);
        bool taken = EvaluateBranchCondition(opcode.branch_condition, value);
        if (taken) {
            // Ignore the delay slot if the branch has the annul bit.
            if (opcode.branch_annul) {
                pc = base_address + opcode.GetBranchTarget();
                return true;
            }

            delayed_pc = base_address + opcode.GetBranchTarget();
            // Execute one more instruction due to the delay slot.
            return Step(true);
        }
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented macro operation {}", opcode.operation.Value());
        break;
    }

    // An instruction with the Exit flag will not actually
    // cause an exit if it's executed inside a delay slot.
    if (opcode.is_exit && !is_delay_slot) {
        // Exit has a delay slot, execute the next instruction
        Step(true);
        return false;
    }

    return true;
}

u32 MacroInterpreterImpl::GetALUResult(Macro::ALUOperation operation, u32 src_a, u32 src_b) {
    switch (operation) {
    case Macro::ALUOperation::Add: {
        const u64 result{static_cast<u64>(src_a) + src_b};
        carry_flag = result > 0xffffffff;
        return static_cast<u32>(result);
    }
    case Macro::ALUOperation::AddWithCarry: {
        const u64 result{static_cast<u64>(src_a) + src_b + (carry_flag ? 1ULL : 0ULL)};
        carry_flag = result > 0xffffffff;
        return static_cast<u32>(result);
    }
    case Macro::ALUOperation::Subtract: {
        const u64 result{static_cast<u64>(src_a) - src_b};
        carry_flag = result < 0x100000000;
        return static_cast<u32>(result);
    }
    case Macro::ALUOperation::SubtractWithBorrow: {
        const u64 result{static_cast<u64>(src_a) - src_b - (carry_flag ? 0ULL : 1ULL)};
        carry_flag = result < 0x100000000;
        return static_cast<u32>(result);
    }
    case Macro::ALUOperation::Xor:
        return src_a ^ src_b;
    case Macro::ALUOperation::Or:
        return src_a | src_b;
    case Macro::ALUOperation::And:
        return src_a & src_b;
    case Macro::ALUOperation::AndNot:
        return src_a & ~src_b;
    case Macro::ALUOperation::Nand:
        return ~(src_a & src_b);

    default:
        UNIMPLEMENTED_MSG("Unimplemented ALU operation {}", operation);
        return 0;
    }
}

void MacroInterpreterImpl::ProcessResult(Macro::ResultOperation operation, u32 reg, u32 result) {
    switch (operation) {
    case Macro::ResultOperation::IgnoreAndFetch:
        // Fetch parameter and ignore result.
        SetRegister(reg, FetchParameter());
        break;
    case Macro::ResultOperation::Move:
        // Move result.
        SetRegister(reg, result);
        break;
    case Macro::ResultOperation::MoveAndSetMethod:
        // Move result and use as Method Address.
        SetRegister(reg, result);
        SetMethodAddress(result);
        break;
    case Macro::ResultOperation::FetchAndSend:
        // Fetch parameter and send result.
        SetRegister(reg, FetchParameter());
        Send(result);
        break;
    case Macro::ResultOperation::MoveAndSend:
        // Move and send result.
        SetRegister(reg, result);
        Send(result);
        break;
    case Macro::ResultOperation::FetchAndSetMethod:
        // Fetch parameter and use result as Method Address.
        SetRegister(reg, FetchParameter());
        SetMethodAddress(result);
        break;
    case Macro::ResultOperation::MoveAndSetMethodFetchAndSend:
        // Move result and use as Method Address, then fetch and send parameter.
        SetRegister(reg, result);
        SetMethodAddress(result);
        Send(FetchParameter());
        break;
    case Macro::ResultOperation::MoveAndSetMethodSend:
        // Move result and use as Method Address, then send bits 12:17 of result.
        SetRegister(reg, result);
        SetMethodAddress(result);
        Send((result >> 12) & 0b111111);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented result operation {}", operation);
        break;
    }
}

bool MacroInterpreterImpl::EvaluateBranchCondition(Macro::BranchCondition cond, u32 value) const {
    switch (cond) {
    case Macro::BranchCondition::Zero:
        return value == 0;
    case Macro::BranchCondition::NotZero:
        return value != 0;
    }
    UNREACHABLE();
}

Macro::Opcode MacroInterpreterImpl::GetOpcode() const {
    // Every PC update is a multiple of four, including branch targets. Avoid a hot-path
    // assertion call in release builds while retaining the bounds check below.
    DEBUG_ASSERT((pc % sizeof(u32)) == 0);
    // ASSERT is implemented as a non-inlined lambda. Check bounds in the hot path, but only
    // enter the reporting function for an invalid program counter.
    if (pc >= code.size() * sizeof(u32)) [[unlikely]] {
        ASSERT_MSG(false, "MME macro program counter is out of bounds");
        // Asserts may be non-fatal: end the macro instead of reading past its code.
        Macro::Opcode exit{};
        exit.operation.Assign(Macro::Operation::AddImmediate);
        exit.result_operation.Assign(Macro::ResultOperation::Move);
        exit.is_exit.Assign(1);
        return exit;
    }
    return {code[pc / sizeof(u32)]};
}

u32 MacroInterpreterImpl::GetRegister(u32 register_id) const {
    // Opcode register selectors are 3-bit fields, so all values are in [0, 7].
    static_assert(Macro::NUM_MACRO_REGISTERS == 8);
    return registers[register_id];
}

void MacroInterpreterImpl::SetRegister(u32 register_id, u32 value) {
    // Register 0 is hardwired as the zero register.
    // Ensure no writes to it actually occur.
    if (register_id == 0) {
        return;
    }

    registers[register_id] = value;
}

void MacroInterpreterImpl::SetMethodAddress(u32 address) {
    method_address.raw = address;
}

void MacroInterpreterImpl::Send(u32 value) {
    // A write to a macro method can recursively invoke ProcessMacro and reallocate or clear the
    // caller's macro_params vector. Preserve the original FIFO only for this uncommon case.
    if (method_address.address.Value() >= 0xE00 && !parameters_snapshotted) {
        stable_parameters.assign(parameters, parameters + num_parameters);
        parameters = stable_parameters.data();
        parameters_snapshotted = true;
    }
    maxwell3d.CallMethod(method_address.address, value, true);
    // Increment the method address by the method increment.
    method_address.address.Assign(method_address.address.Value() +
                                  method_address.increment.Value());
}

u32 MacroInterpreterImpl::Read(u32 method) const {
    return maxwell3d.GetRegisterValue(method);
}

u32 MacroInterpreterImpl::FetchParameter() {
    if (next_parameter_index >= num_parameters) [[unlikely]] {
        ASSERT_MSG(false, "MME macro fetched beyond its parameter FIFO");
        return 0;
    }
    return parameters[next_parameter_index++];
}
} // Anonymous namespace

MacroInterpreter::MacroInterpreter(Engines::Maxwell3D& maxwell3d_)
    : MacroEngine{maxwell3d_}, maxwell3d{maxwell3d_} {}

std::unique_ptr<CachedMacro> MacroInterpreter::Compile(const std::vector<u32>& code) {
    return std::make_unique<MacroInterpreterImpl>(maxwell3d, code);
}

} // namespace Tegra
