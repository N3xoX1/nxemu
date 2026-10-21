// SPDX-FileCopyrightText: Copyright 2026 nxemu Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yuzu_shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization
{

// Branch-sensitive handle tracking, inspired by Eden PR #4551 (xbzk).
// Use the structured program rather than rediscovering a particular diamond
// shape for every operand. Facts persist through straight-line and nested blocks.
class ConditionalValueResolver
{
public:
    explicit ConditionalValueResolver(const IR::Program & program_) :
        program{program_}
    {
    }

    void BeginBlock(const IR::Block * block)
    {
        use = block;
        facts_ready = false;
        facts.clear();
        possible.clear();
    }

    std::optional<bool> Truth(IR::Value value)
    {
        const auto [condition, negated] = Normalize(value);
        if (condition.IsImmediate())
        {
            if (condition.Type() == IR::Type::U1) return condition.U1() != negated;
            return std::nullopt;
        }
        PrepareFacts();
        if (const auto it = facts.find(Key(condition.Inst())); it != facts.end())
        {
            if (it->second < 2) return (it->second != 0) != negated;
        }
        return std::nullopt;
    }

    bool IncomingPossible(const IR::Inst * phi, size_t index)
    {
        PrepareFacts();
        const auto owner = phi_blocks.find(phi);
        const auto current = blocks.find(use);
        const auto source = blocks.find(phi->PhiBlock(index));
        if (owner == phi_blocks.end() || current == blocks.end() || source == blocks.end() ||
            loop_headers.contains(owner->second) ||
            blocks.at(owner->second).loop != current->second.loop ||
            source->second.loop != current->second.loop)
        {
            // A predicate's SSA instruction can have a different dynamic value
            // in another loop iteration. Never prune loop-carried edges with it.
            return true;
        }
        const Edge edge{phi->PhiBlock(index), owner->second};
        if (const auto it = possible.find(edge); it != possible.end()) return it->second;
        bool result = true;
        for (size_t scope = source->second.scope; scope != 0; scope = scopes[scope].parent)
        {
            if (const auto truth = Truth(scopes[scope].condition); truth && *truth != scopes[scope].truth)
            {
                result = false;
                break;
            }
        }
        if (result)
        {
            if (const auto it = branches.find(edge.from); it != branches.end())
            {
                const auto & branch = it->second;
                if (branch.yes != branch.no && (edge.to == branch.yes || edge.to == branch.no))
                {
                    if (const auto truth = Truth(branch.condition))
                    {
                        result = *truth == (edge.to == branch.yes);
                    }
                }
            }
        }
        possible.emplace(edge, result);
        return result;
    }

private:
    struct Scope
    {
        size_t parent;
        IR::Value condition;
        bool truth;
    };
    struct BlockInfo
    {
        size_t scope;
        size_t loop;
    };
    struct Branch
    {
        IR::Value condition;
        const IR::Block * yes;
        const IR::Block * no;
    };
    struct Edge
    {
        const IR::Block * from;
        const IR::Block * to;
        bool operator==(const Edge &) const = default;
    };
    struct EdgeHash
    {
        size_t operator()(const Edge & e) const
        {
            return std::hash<const void *>{}(e.from) ^ (std::hash<const void *>{}(e.to) << 1);
        }
    };

    struct OperandKey
    {
        uintptr_t value{}, offset{};
        unsigned kind{}; // SSA expression, immediate, or constant-buffer word.
        bool operator==(const OperandKey &) const = default;
    };
    struct ConditionKey
    {
        IR::Opcode opcode{};
        OperandKey lhs{}, rhs{};
        bool operator==(const ConditionKey &) const = default;
    };
    struct ConditionHash
    {
        size_t operator()(const ConditionKey & key) const
        {
            size_t hash = static_cast<size_t>(key.opcode);
            for (const auto & operand : {key.lhs, key.rhs})
            {
                for (const auto part : {operand.value, operand.offset, uintptr_t{operand.kind}})
                    hash ^= part + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    static OperandKey Operand(IR::Value value)
    {
        value = value.Resolve();
        if (value.IsImmediate()) return {value.U32(), 0, 1};
        const auto * inst = value.Inst();
        if (inst->GetOpcode() == IR::Opcode::GetCbufU32 &&
            inst->Arg(0).IsImmediate() && inst->Arg(1).IsImmediate())
            return {inst->Arg(0).U32(), inst->Arg(1).U32(), 2};
        return {reinterpret_cast<uintptr_t>(inst), 0, 0};
    }

    // Repeated reads of the same constant-buffer word are equivalent. This
    // recognizes re-emitted integer predicates without a whole-shader CSE pass.
    static ConditionKey Key(const IR::Inst * inst)
    {
        auto opcode = inst->GetOpcode();
        switch (opcode)
        {
        case IR::Opcode::IEqual:
        case IR::Opcode::INotEqual:
        case IR::Opcode::ULessThan:
        case IR::Opcode::ULessThanEqual:
        case IR::Opcode::UGreaterThan:
        case IR::Opcode::UGreaterThanEqual:
        case IR::Opcode::SLessThan:
        case IR::Opcode::SLessThanEqual:
        case IR::Opcode::SGreaterThan:
        case IR::Opcode::SGreaterThanEqual:
            break;
        default:
            return {IR::Opcode::Void, {reinterpret_cast<uintptr_t>(inst), 0, 0}, {}};
        }
        auto lhs = Operand(inst->Arg(0));
        auto rhs = Operand(inst->Arg(1));
        switch (opcode)
        {
        case IR::Opcode::INotEqual: opcode = IR::Opcode::IEqual; break;
        case IR::Opcode::UGreaterThanEqual: opcode = IR::Opcode::ULessThan; break;
        case IR::Opcode::SGreaterThanEqual: opcode = IR::Opcode::SLessThan; break;
        case IR::Opcode::ULessThanEqual:
        case IR::Opcode::UGreaterThan:
            opcode = IR::Opcode::ULessThan;
            std::swap(lhs, rhs);
            break;
        case IR::Opcode::SLessThanEqual:
        case IR::Opcode::SGreaterThan:
            opcode = IR::Opcode::SLessThan;
            std::swap(lhs, rhs);
            break;
        default: break;
        }
        if (opcode == IR::Opcode::IEqual &&
            std::tie(lhs.kind, lhs.value, lhs.offset) > std::tie(rhs.kind, rhs.value, rhs.offset))
            std::swap(lhs, rhs);
        return {opcode, lhs, rhs};
    }

    static std::pair<IR::Value, bool> Normalize(IR::Value value)
    {
        bool negated{};
        while (const auto * inst = value.TryInstRecursive())
        {
            if (inst->GetOpcode() == IR::Opcode::IEqual || inst->GetOpcode() == IR::Opcode::INotEqual)
            {
                IR::Value constant = inst->Arg(0).Resolve();
                IR::Value operand = inst->Arg(1).Resolve();
                if (!constant.IsImmediate()) std::swap(constant, operand);
                const auto * select = operand.TryInstRecursive();
                if (!constant.IsImmediate() || !select || select->GetOpcode() != IR::Opcode::SelectU32 ||
                    !select->Arg(1).IsImmediate() || !select->Arg(2).IsImmediate()) break;
                const bool equal = inst->GetOpcode() == IR::Opcode::IEqual;
                const bool yes = (select->Arg(1).U32() == constant.U32()) == equal;
                const bool no = (select->Arg(2).U32() == constant.U32()) == equal;
                if (yes == no)
                {
                    value = IR::Value{yes};
                    break;
                }
                negated ^= !yes;
                value = select->Arg(0);
                continue;
            }
            if (inst->GetOpcode() == IR::Opcode::LogicalNot)
                negated = !negated;
            else if (inst->GetOpcode() != IR::Opcode::ConditionRef)
                break;
            value = inst->Arg(0);
        }
        value = value.Resolve();
        if (const auto * inst = value.TryInstRecursive())
        {
            switch (inst->GetOpcode())
            {
            case IR::Opcode::INotEqual:
            case IR::Opcode::ULessThanEqual:
            case IR::Opcode::UGreaterThanEqual:
            case IR::Opcode::SLessThanEqual:
            case IR::Opcode::SGreaterThanEqual:
                negated = !negated;
                break;
            default: break;
            }
        }
        return {value, negated};
    }

    void PrepareFacts()
    {
        if (facts_ready) return;
        if (!prepared) Prepare();
        facts_ready = true;
        const auto block = blocks.find(use);
        if (block == blocks.end()) return;
        for (size_t scope = block->second.scope; scope != 0; scope = scopes[scope].parent)
        {
            const auto [condition, negated] = Normalize(scopes[scope].condition);
            if (condition.IsImmediate()) continue;
            const unsigned truth = scopes[scope].truth != negated;
            const auto [it, inserted] = facts.emplace(Key(condition.Inst()), truth);
            if (!inserted && it->second != truth) it->second = 2; // Contradictory/unreachable path.
        }
    }

    void Prepare()
    {
        prepared = true;
        scopes.push_back({0, {}, false});
        size_t scope{}, loop{}, next_loop{};
        std::vector<size_t> if_scopes;
        std::vector<BlockInfo> loops;
        const IR::Block * previous{};
        for (const auto & node : program.syntax_list)
        {
            using Type = IR::AbstractSyntaxNode::Type;
            switch (node.type)
            {
            case Type::Block:
                previous = node.data.block;
                blocks.emplace(previous, BlockInfo{scope, loop});
                for (const auto & inst : previous->Instructions())
                {
                    if (inst.GetOpcode() == IR::Opcode::Phi)
                        phi_blocks.emplace(&inst, previous);
                    else if (inst.GetOpcode() != IR::Opcode::Identity &&
                             inst.GetOpcode() != IR::Opcode::UndefU1 &&
                             inst.GetOpcode() != IR::Opcode::UndefU32 &&
                             inst.GetOpcode() != IR::Opcode::UndefU64)
                        break; // SSA phis precede the ordinary instructions.
                }
                break;
            case Type::If:
                branches.emplace(previous, Branch{node.data.if_node.cond,
                                                  node.data.if_node.body, node.data.if_node.merge});
                if_scopes.push_back(scope);
                scopes.push_back({scope, node.data.if_node.cond, true});
                scope = scopes.size() - 1;
                break;
            case Type::EndIf:
                scope = if_scopes.back();
                if_scopes.pop_back();
                break;
            case Type::Loop:
                loops.push_back({scope, loop});
                loop = ++next_loop;
                loop_headers.insert(previous);
                break;
            case Type::Repeat:
                branches.emplace(previous, Branch{node.data.repeat.cond,
                                                  node.data.repeat.loop_header, node.data.repeat.merge});
                scope = loops.back().scope;
                loop = loops.back().loop;
                loops.pop_back();
                break;
            case Type::Break:
                branches.emplace(previous, Branch{node.data.break_node.cond,
                                                  node.data.break_node.merge, node.data.break_node.skip});
                scopes.push_back({scope, node.data.break_node.cond, false});
                scope = scopes.size() - 1;
                break;
            default:
                break;
            }
        }
    }

    const IR::Program & program;
    const IR::Block * use{};
    bool prepared{}, facts_ready{};
    std::vector<Scope> scopes;
    std::unordered_map<const IR::Block *, BlockInfo> blocks;
    std::unordered_map<const IR::Inst *, const IR::Block *> phi_blocks;
    std::unordered_map<const IR::Block *, Branch> branches;
    std::unordered_set<const IR::Block *> loop_headers;
    std::unordered_map<ConditionKey, unsigned, ConditionHash> facts;
    std::unordered_map<Edge, bool, EdgeHash> possible;
};

} // namespace Shader::Optimization
