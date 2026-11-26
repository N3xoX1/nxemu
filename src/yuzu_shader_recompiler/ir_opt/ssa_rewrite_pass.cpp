// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// This file implements the SSA rewriting algorithm proposed in
//
//      Simple and Efficient Construction of Static Single Assignment Form.
//      Braun M., Buchwald S., Hack S., Leiba R., Mallon C., Zwinkau A. (2013)
//      In: Jhala R., De Bosschere K. (eds)
//      Compiler Construction. CC 2013.
//      Lecture Notes in Computer Science, vol 7791.
//      Springer, Berlin, Heidelberg
//
//      https://link.springer.com/chapter/10.1007/978-3-642-37051-9_6
//

#include <deque>
#include <map>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/opcodes.h"
#include "yuzu_shader_recompiler/frontend/ir/pred.h"
#include "yuzu_shader_recompiler/frontend/ir/reg.h"
#include "yuzu_shader_recompiler/frontend/ir/value.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
struct FlagTag {
    auto operator<=>(const FlagTag&) const noexcept = default;
};
struct ZeroFlagTag : FlagTag {};
struct SignFlagTag : FlagTag {};
struct CarryFlagTag : FlagTag {};
struct OverflowFlagTag : FlagTag {};

struct GotoVariable : FlagTag {
    GotoVariable() = default;
    explicit GotoVariable(u32 index_) : index{index_} {}

    auto operator<=>(const GotoVariable&) const noexcept = default;

    u32 index;
};

struct IndirectBranchVariable {
    auto operator<=>(const IndirectBranchVariable&) const noexcept = default;
};

using Variant = std::variant<IR::Reg, IR::Pred, ZeroFlagTag, SignFlagTag, CarryFlagTag,
                             OverflowFlagTag, GotoVariable, IndirectBranchVariable>;
using ValueMap = std::unordered_map<IR::Block*, IR::Value>;

struct DefTable {
    const IR::Value& Def(IR::Block* block, IR::Reg variable) {
        return block->SsaRegValue(variable);
    }
    void SetDef(IR::Block* block, IR::Reg variable, const IR::Value& value) {
        block->SetSsaRegValue(variable, value);
    }

    const IR::Value& Def(IR::Block* block, IR::Pred variable) {
        return preds[IR::PredIndex(variable)][block];
    }
    void SetDef(IR::Block* block, IR::Pred variable, const IR::Value& value) {
        preds[IR::PredIndex(variable)].insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, GotoVariable variable) {
        return goto_vars[variable.index][block];
    }
    void SetDef(IR::Block* block, GotoVariable variable, const IR::Value& value) {
        goto_vars[variable.index].insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, IndirectBranchVariable) {
        return indirect_branch_var[block];
    }
    void SetDef(IR::Block* block, IndirectBranchVariable, const IR::Value& value) {
        indirect_branch_var.insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, ZeroFlagTag) {
        return zero_flag[block];
    }
    void SetDef(IR::Block* block, ZeroFlagTag, const IR::Value& value) {
        zero_flag.insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, SignFlagTag) {
        return sign_flag[block];
    }
    void SetDef(IR::Block* block, SignFlagTag, const IR::Value& value) {
        sign_flag.insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, CarryFlagTag) {
        return carry_flag[block];
    }
    void SetDef(IR::Block* block, CarryFlagTag, const IR::Value& value) {
        carry_flag.insert_or_assign(block, value);
    }

    const IR::Value& Def(IR::Block* block, OverflowFlagTag) {
        return overflow_flag[block];
    }
    void SetDef(IR::Block* block, OverflowFlagTag, const IR::Value& value) {
        overflow_flag.insert_or_assign(block, value);
    }

    std::array<ValueMap, IR::NUM_USER_PREDS> preds;
    std::unordered_map<u32, ValueMap> goto_vars;
    ValueMap indirect_branch_var;
    ValueMap zero_flag;
    ValueMap sign_flag;
    ValueMap carry_flag;
    ValueMap overflow_flag;
};

IR::Opcode UndefOpcode(IR::Reg) noexcept {
    return IR::Opcode::UndefU32;
}

IR::Opcode UndefOpcode(IR::Pred) noexcept {
    return IR::Opcode::UndefU1;
}

IR::Opcode UndefOpcode(const FlagTag&) noexcept {
    return IR::Opcode::UndefU1;
}

IR::Opcode UndefOpcode(IndirectBranchVariable) noexcept {
    return IR::Opcode::UndefU32;
}

enum class Status {
    Start,
    SetValue,
    PreparePhiArgument,
    PushPhiArgument,
};

struct ReadState {
    ReadState(IR::Block* block_) : block{block_} {}
    ReadState() = default;

    IR::Block* block{};
    IR::Value result{};
    /// Phi of a sealed block with several predecessors, null while it is only pending
    IR::Inst* phi{};
    IR::Block* const* pred_it{};
    IR::Block* const* pred_end{};
    /// Operands of a pending phi, in Pass::pending_operands
    size_t operands_begin{};
    size_t num_operands{};
    Status pc{Status::Start};
};

class Pass {
public:
    template <typename Type>
    void WriteVariable(Type variable, IR::Block* block, const IR::Value& value) {
        current_def.SetDef(block, variable, value);
    }

    template <typename Type>
    IR::Value ReadVariable(Type variable, IR::Block* root_block) {
        // Reads walk back through every block until they find a definition, often dozens deep:
        // reuse one stack instead of growing a new vector on every read.
        auto& stack{read_stack};
        stack.clear();
        stack.emplace_back(nullptr);
        stack.emplace_back(root_block);
        const IR::Opcode undef_opcode{UndefOpcode(variable)};
        const auto prepare_phi_operand{[&] {
            ReadState& state{stack.back()};
            if (state.pred_it == state.pred_end) {
                IR::Block* const block{state.block};
                const IR::Value result{state.phi
                                           ? TryRemoveTrivialPhi(*state.phi, block, undef_opcode)
                                           : ResolvePendingPhi(state, undef_opcode)};
                pending_operands.resize(state.operands_begin);
                stack.pop_back();
                stack.back().result = result;
                WriteVariable(variable, block, result);
            } else {
                IR::Block* const imm_pred{*state.pred_it};
                state.pc = Status::PushPhiArgument;
                stack.emplace_back(imm_pred);
            }
        }};
        do {
            IR::Block* const block{stack.back().block};
            switch (stack.back().pc) {
            case Status::Start: {
                if (IR::Value def{current_def.Def(block, variable)}; !def.IsEmpty()) {
                    if (def == pending_phi) {
                        // A cycle reached a pending phi: it needs to exist now
                        def = IR::Value{MaterializePhi(PendingFrame(block), undef_opcode)};
                        WriteVariable(variable, block, def);
                    }
                    stack.back().result = def;
                } else if (!block->IsSsaSealed()) {
                    // Incomplete CFG
                    IR::Inst* phi{&*block->PrependNewInst(block->begin(), IR::Opcode::Phi)};
                    phi->SetFlags(IR::TypeOf(undef_opcode));

                    incomplete_phis[block].insert_or_assign(variable, phi);
                    stack.back().result = IR::Value{&*phi};
                } else if (const std::span imm_preds = block->ImmPredecessors();
                           imm_preds.size() == 1) {
                    // Optimize the common case of one predecessor: no phi needed
                    stack.back().pc = Status::SetValue;
                    stack.emplace_back(imm_preds.front());
                    break;
                } else {
                    // Break potential cycles with an operandless phi. Most of these phis turn out
                    // trivial, so it stays pending (pending_phi marks the block) and is only
                    // created when a cycle reads it or when it merges different values. Nothing
                    // is inserted at the front of this block until then, so it is created where
                    // it would have been created right away.
                    WriteVariable(variable, block, pending_phi);

                    stack.back().phi = nullptr;
                    stack.back().operands_begin = pending_operands.size();
                    stack.back().num_operands = 0;
                    stack.back().pred_it = imm_preds.data();
                    stack.back().pred_end = imm_preds.data() + imm_preds.size();
                    prepare_phi_operand();
                    break;
                }
            }
                [[fallthrough]];
            case Status::SetValue: {
                const IR::Value result{stack.back().result};
                WriteVariable(variable, block, result);
                stack.pop_back();
                stack.back().result = result;
                break;
            }
            case Status::PushPhiArgument: {
                ReadState& state{stack.back()};
                if (state.phi) {
                    state.phi->AddPhiOperand(*state.pred_it, state.result);
                } else {
                    pending_operands.emplace_back(*state.pred_it, state.result);
                    ++state.num_operands;
                }
                ++state.pred_it;
            }
                [[fallthrough]];
            case Status::PreparePhiArgument:
                prepare_phi_operand();
                break;
            }
        } while (stack.size() > 1);
        return stack.back().result;
    }

    void SealBlock(IR::Block* block) {
        const auto it{incomplete_phis.find(block)};
        if (it != incomplete_phis.end()) {
            for (auto& pair : it->second) {
                auto& variant{pair.first};
                auto& phi{pair.second};
                std::visit([&](auto& variable) { AddPhiOperands(variable, *phi, block); }, variant);
            }
        }
        block->SsaSeal();
    }

private:
    ReadState& PendingFrame(IR::Block* block) {
        for (auto it = read_stack.rbegin(); it != read_stack.rend(); ++it) {
            if (it->block == block && it->pc == Status::PushPhiArgument && !it->phi) {
                return *it;
            }
        }
        throw LogicError("Pending phi without a frame");
    }

    /// Creates the phi of a pending frame with the operands it collected so far
    IR::Inst* MaterializePhi(ReadState& state, IR::Opcode undef_opcode) {
        IR::Block* const block{state.block};
        IR::Inst* const phi{&*block->PrependNewInst(block->begin(), IR::Opcode::Phi)};
        phi->SetFlags(IR::TypeOf(undef_opcode));
        for (size_t i = 0; i < state.num_operands; ++i) {
            const auto& [pred, value]{pending_operands[state.operands_begin + i]};
            phi->AddPhiOperand(pred, value);
        }
        state.phi = phi;
        return phi;
    }

    /// TryRemoveTrivialPhi for a phi nothing has seen yet: no self-references, and nothing to
    /// remove or reroute when it is trivial
    IR::Value ResolvePendingPhi(ReadState& state, IR::Opcode undef_opcode) {
        IR::Value same;
        for (size_t i = 0; i < state.num_operands; ++i) {
            const IR::Value& op{pending_operands[state.operands_begin + i].second};
            if (op.Resolve() == same.Resolve()) {
                continue;
            }
            if (!same.IsEmpty()) {
                return IR::Value{MaterializePhi(state, undef_opcode)};
            }
            same = op;
        }
        if (same.IsEmpty()) {
            // Unreachable, or the start block: same insertion point as in TryRemoveTrivialPhi
            IR::Block* const block{state.block};
            const auto insert_point{std::ranges::find_if_not(block->Instructions(), IR::IsPhi)};
            same = IR::Value{&*block->PrependNewInst(insert_point, undef_opcode)};
        }
        return same;
    }
    template <typename Type>
    IR::Value AddPhiOperands(Type variable, IR::Inst& phi, IR::Block* block) {
        for (IR::Block* const imm_pred : block->ImmPredecessors()) {
            phi.AddPhiOperand(imm_pred, ReadVariable(variable, imm_pred));
        }
        return TryRemoveTrivialPhi(phi, block, UndefOpcode(variable));
    }

    IR::Value TryRemoveTrivialPhi(IR::Inst& phi, IR::Block* block, IR::Opcode undef_opcode) {
        IR::Value same;
        const size_t num_args{phi.NumArgs()};
        for (size_t arg_index = 0; arg_index < num_args; ++arg_index) {
            const IR::Value& op{phi.Arg(arg_index)};
            if (op.Resolve() == same.Resolve() || op == IR::Value{&phi}) {
                // Unique value or self-reference
                continue;
            }
            if (!same.IsEmpty()) {
                // The phi merges at least two values: not trivial
                return IR::Value{&phi};
            }
            same = op;
        }
        // Remove the phi node from the block, it will be reinserted
        IR::Block::InstructionList& list{block->Instructions()};
        list.erase(IR::Block::InstructionList::s_iterator_to(phi));

        // Find the first non-phi instruction and use it as an insertion point
        IR::Block::iterator reinsert_point{std::ranges::find_if_not(list, IR::IsPhi)};
        if (same.IsEmpty()) {
            // The phi is unreachable or in the start block
            // Insert an undefined instruction and make it the phi node replacement
            // The "phi" node reinsertion point is specified after this instruction
            reinsert_point = block->PrependNewInst(reinsert_point, undef_opcode);
            same = IR::Value{&*reinsert_point};
            ++reinsert_point;
        }
        // Reinsert the phi node and reroute all its uses to the "same" value
        list.insert(reinsert_point, phi);
        phi.ReplaceUsesWith(same);
        // TODO: Try to recursively remove all phi users, which might have become trivial
        return same;
    }

    std::unordered_map<IR::Block*, std::map<Variant, IR::Inst*>> incomplete_phis;
    DefTable current_def;
    // ReadVariable never nests, so its walk stack can be shared
    std::vector<ReadState> read_stack;
    std::vector<std::pair<IR::Block*, IR::Value>> pending_operands;
    // Definition of blocks whose phi is pending
    IR::Inst pending_phi_inst{IR::Opcode::Void, 0};
    const IR::Value pending_phi{&pending_phi_inst};
};

void VisitInst(Pass& pass, IR::Block* block, IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::SetRegister:
        if (const IR::Reg reg{inst.Arg(0).Reg()}; reg != IR::Reg::RZ) {
            pass.WriteVariable(reg, block, inst.Arg(1));
        }
        break;
    case IR::Opcode::SetPred:
        if (const IR::Pred pred{inst.Arg(0).Pred()}; pred != IR::Pred::PT) {
            pass.WriteVariable(pred, block, inst.Arg(1));
        }
        break;
    case IR::Opcode::SetGotoVariable:
        pass.WriteVariable(GotoVariable{inst.Arg(0).U32()}, block, inst.Arg(1));
        break;
    case IR::Opcode::SetIndirectBranchVariable:
        pass.WriteVariable(IndirectBranchVariable{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetZFlag:
        pass.WriteVariable(ZeroFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetSFlag:
        pass.WriteVariable(SignFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetCFlag:
        pass.WriteVariable(CarryFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetOFlag:
        pass.WriteVariable(OverflowFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::GetRegister:
        if (const IR::Reg reg{inst.Arg(0).Reg()}; reg != IR::Reg::RZ) {
            inst.ReplaceUsesWith(pass.ReadVariable(reg, block));
        }
        break;
    case IR::Opcode::GetPred:
        if (const IR::Pred pred{inst.Arg(0).Pred()}; pred != IR::Pred::PT) {
            inst.ReplaceUsesWith(pass.ReadVariable(pred, block));
        }
        break;
    case IR::Opcode::GetGotoVariable:
        inst.ReplaceUsesWith(pass.ReadVariable(GotoVariable{inst.Arg(0).U32()}, block));
        break;
    case IR::Opcode::GetIndirectBranchVariable:
        inst.ReplaceUsesWith(pass.ReadVariable(IndirectBranchVariable{}, block));
        break;
    case IR::Opcode::GetZFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(ZeroFlagTag{}, block));
        break;
    case IR::Opcode::GetSFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(SignFlagTag{}, block));
        break;
    case IR::Opcode::GetCFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(CarryFlagTag{}, block));
        break;
    case IR::Opcode::GetOFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(OverflowFlagTag{}, block));
        break;
    default:
        break;
    }
}

void VisitBlock(Pass& pass, IR::Block* block) {
    for (IR::Inst& inst : block->Instructions()) {
        VisitInst(pass, block, inst);
    }
    pass.SealBlock(block);
}

IR::Type GetConcreteType(IR::Inst* inst) {
    std::deque<IR::Inst*> queue;
    queue.push_back(inst);
    while (!queue.empty()) {
        IR::Inst* current = queue.front();
        queue.pop_front();
        const size_t num_args{current->NumArgs()};
        for (size_t i = 0; i < num_args; ++i) {
            const auto set_type = current->Arg(i).Type();
            if (set_type != IR::Type::Opaque) {
                return set_type;
            }
            if (!current->Arg(i).IsImmediate()) {
                queue.push_back(current->Arg(i).Inst());
            }
        }
    }
    return IR::Type::Opaque;
}
} // Anonymous namespace

void SsaRewritePass(IR::Program& program) {
    Pass pass;
    const auto end{program.post_order_blocks.rend()};
    for (auto block = program.post_order_blocks.rbegin(); block != end; ++block) {
        VisitBlock(pass, *block);
    }
    for (auto block = program.post_order_blocks.rbegin(); block != end; ++block) {
        for (IR::Inst& inst : (*block)->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Phi) {
                if (inst.Type() == IR::Type::Opaque) {
                    inst.SetFlags(GetConcreteType(&inst));
                }
                inst.OrderPhiArgs();
            }
        }
    }
    // Trivial phis and replaced reads stay in their blocks as Identity instructions, often more
    // of them than real instructions. Those without uses cannot gain any now that the definition
    // tables are gone: drop them here, as dead code elimination would later (same order), instead
    // of letting every pass until then walk them.
    for (IR::Block* const block : program.post_order_blocks) {
        auto it{block->end()};
        while (it != block->begin()) {
            --it;
            if (it->GetOpcode() == IR::Opcode::Identity && !it->HasUses()) {
                it->Invalidate();
                it = block->Instructions().erase(it);
            }
        }
    }
}

} // namespace Shader::Optimization
