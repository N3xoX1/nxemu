/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/basic_block.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>
#include <type_traits>

#include <fmt/format.h>
#include <mcl/assert.hpp>

#include "dynarmic/common/memory_pool.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::IR {

static_assert(std::is_trivially_destructible_v<Inst>,
              "IR instruction pool reuse requires trivially destructible Inst objects");

Block::Block(const LocationDescriptor& location)
        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 64)} {}

Block::~Block() = default;

Block::Block(Block&&) = default;

Block& Block::operator=(Block&&) = default;


void Block::Reset(const LocationDescriptor& new_location) {
    while (!instructions.empty()) {
        instructions.pop_front();
    }
    instruction_alloc_pool->Reset();

    location = new_location;
    end_location = new_location;
    cond = Cond::AL;
    cond_failed.reset();
    cond_failed_cycle_count = 0;
    terminal = Term::Invalid{};
    cycle_count = 0;
}

void Block::AppendNewInst(Opcode opcode, std::initializer_list<IR::Value> args) {
    PrependNewInst(end(), opcode, args);
}

Block::iterator Block::PrependNewInst(iterator insertion_point, Opcode opcode, std::initializer_list<Value> args) {
    IR::Inst* inst = new (instruction_alloc_pool->Alloc()) IR::Inst(opcode);
    ASSERT(args.size() == inst->NumArgs());

    std::for_each(args.begin(), args.end(), [&inst, index = size_t(0)](const auto& arg) mutable {
        inst->SetArg(index, arg);
        index++;
    });

    return instructions.insert_before(insertion_point, inst);
}

LocationDescriptor Block::Location() const {
    return location;
}

LocationDescriptor Block::EndLocation() const {
    return end_location;
}

void Block::SetEndLocation(const LocationDescriptor& descriptor) {
    end_location = descriptor;
}

Cond Block::GetCondition() const {
    return cond;
}

void Block::SetCondition(Cond condition) {
    cond = condition;
}

LocationDescriptor Block::ConditionFailedLocation() const {
    return *cond_failed;
}

void Block::SetConditionFailedLocation(LocationDescriptor fail_location) {
    cond_failed = fail_location;
}

size_t& Block::ConditionFailedCycleCount() {
    return cond_failed_cycle_count;
}

const size_t& Block::ConditionFailedCycleCount() const {
    return cond_failed_cycle_count;
}

bool Block::HasConditionFailedLocation() const {
    return cond_failed.has_value();
}

Block::InstructionList& Block::Instructions() {
    return instructions;
}

const Block::InstructionList& Block::Instructions() const {
    return instructions;
}

Terminal Block::GetTerminal() const {
    return terminal;
}

void Block::SetTerminal(Terminal term) {
    ASSERT_MSG(!HasTerminal(), "Terminal has already been set.");
    terminal = std::move(term);
}

void Block::ReplaceTerminal(Terminal term) {
    ASSERT_MSG(HasTerminal(), "Terminal has not been set.");
    terminal = std::move(term);
}

bool Block::HasTerminal() const {
    return terminal.which() != 0;
}

size_t& Block::CycleCount() {
    return cycle_count;
}

const size_t& Block::CycleCount() const {
    return cycle_count;
}

static std::string TerminalToString(const Terminal& terminal_variant) {
    struct : boost::static_visitor<std::string> {
        std::string operator()(const Term::Invalid&) const {
            return "<invalid terminal>";
        }
        std::string operator()(const Term::Interpret& terminal) const {
            return fmt::format("Interpret{{{}}}", terminal.next);
        }
        std::string operator()(const Term::ReturnToDispatch&) const {
            return "ReturnToDispatch{}";
        }
        std::string operator()(const Term::LinkBlock& terminal) const {
            return fmt::format("LinkBlock{{{}}}", terminal.next);
        }
        std::string operator()(const Term::LinkBlockFast& terminal) const {
            return fmt::format("LinkBlockFast{{{}}}", terminal.next);
        }
        std::string operator()(const Term::PopRSBHint&) const {
            return "PopRSBHint{}";
        }
        std::string operator()(const Term::FastDispatchHint&) const {
            return "FastDispatchHint{}";
        }
        std::string operator()(const Term::If& terminal) const {
            return fmt::format("If{{{}, {}, {}}}", A64::CondToString(terminal.if_), TerminalToString(terminal.then_), TerminalToString(terminal.else_));
        }
        std::string operator()(const Term::CheckBit& terminal) const {
            return fmt::format("CheckBit{{{}, {}}}", TerminalToString(terminal.then_), TerminalToString(terminal.else_));
        }
        std::string operator()(const Term::CheckHalt& terminal) const {
            return fmt::format("CheckHalt{{{}}}", TerminalToString(terminal.else_));
        }
    } visitor;

    return boost::apply_visitor(visitor, terminal_variant);
}

std::string DumpBlock(const IR::Block& block) {
    std::string ret;

    ret += fmt::format("Block: location={}\n", block.Location());
    ret += fmt::format("cycles={}", block.CycleCount());
    ret += fmt::format(", entry_cond={}", A64::CondToString(block.GetCondition()));
    if (block.GetCondition() != Cond::AL) {
        ret += fmt::format(", cond_fail={}", block.ConditionFailedLocation());
    }
    ret += '\n';

    const auto arg_to_string = [](const IR::Value& arg) -> std::string {
        if (arg.IsEmpty()) {
            return "<null>";
        } else if (!arg.IsImmediate()) {
            if (const unsigned name = arg.GetInst()->GetName()) {
                return fmt::format("%{}", name);
            }
            return fmt::format("%<unnamed inst {:016x}>", reinterpret_cast<u64>(arg.GetInst()));
        }
        switch (arg.GetType()) {
        case Type::U1:
            return fmt::format("#{}", arg.GetU1() ? '1' : '0');
        case Type::U8:
            return fmt::format("#{}", arg.GetU8());
        case Type::U16:
            return fmt::format("#{:#x}", arg.GetU16());
        case Type::U32:
            return fmt::format("#{:#x}", arg.GetU32());
        case Type::U64:
            return fmt::format("#{:#x}", arg.GetU64());
        case Type::A32Reg:
            return A32::RegToString(arg.GetA32RegRef());
        case Type::A32ExtReg:
            return A32::ExtRegToString(arg.GetA32ExtRegRef());
        case Type::A64Reg:
            return A64::RegToString(arg.GetA64RegRef());
        case Type::A64Vec:
            return A64::VecToString(arg.GetA64VecRef());
        default:
            return "<unknown immediate type>";
        }
    };

    for (const auto& inst : block) {
        const Opcode op = inst.GetOpcode();

        ret += fmt::format("[{:016x}] ", reinterpret_cast<u64>(&inst));
        if (GetTypeOf(op) != Type::Void) {
            if (inst.GetName()) {
                ret += fmt::format("%{:<5} = ", inst.GetName());
            } else {
                ret += "noname = ";
            }
        } else {
            ret += "         ";  // '%00000 = ' -> 1 + 5 + 3 = 9 spaces
        }

        ret += GetNameOf(op);

        const size_t arg_count = GetNumArgsOf(op);
        for (size_t arg_index = 0; arg_index < arg_count; arg_index++) {
            const Value arg = inst.GetArg(arg_index);

            ret += arg_index != 0 ? ", " : " ";
            ret += arg_to_string(arg);

            Type actual_type = arg.GetType();
            Type expected_type = GetArgTypeOf(op, arg_index);
            if (!AreTypesCompatible(actual_type, expected_type)) {
                ret += fmt::format("<type error: {} != {}>", GetNameOf(actual_type), GetNameOf(expected_type));
            }
        }

        ret += fmt::format(" (uses: {})", inst.UseCount());

        ret += '\n';
    }

    ret += "terminal = " + TerminalToString(block.GetTerminal()) + '\n';

    return ret;
}

}  // namespace Dynarmic::IR
