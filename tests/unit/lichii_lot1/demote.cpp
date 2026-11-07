// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_common/logging/log.h"
#include "yuzu_shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace IR = Shader::IR;
using Node = IR::AbstractSyntaxNode;
using Type = Node::Type;
void Require(bool condition)
{
    if (!condition) throw std::runtime_error("demote reorder regression");
}
struct Fixture
{
    Shader::ObjectPool<IR::Inst> instructions{2};
    Shader::ObjectPool<IR::Block> blocks{2};
    IR::AbstractSyntaxList syntax_list;
#include "demote.inc"
    IR::Block * Block(bool demote = false, bool epilogue = false)
    {
        auto * block = blocks.Create(instructions);
        if (demote) block->AppendNewInst(IR::Opcode::DemoteToHelperInvocation, {});
        if (epilogue) block->AppendNewInst(IR::Opcode::Epilogue, {});
        syntax_list.push_back({.data = {.block = block}, .type = Type::Block});
        return block;
    }
    void If(IR::Block * body, IR::Block * merge)
    {
        syntax_list.push_back({.data = {.if_node = {IR::U1{IR::Value{true}}, body, merge}},
                               .type = Type::If});
    }
    void EndIf(IR::Block * merge)
    {
        syntax_list.push_back({.data = {.end_if = {merge}}, .type = Type::EndIf});
    }
};
void TestNestedExit(bool same_block)
{
    Fixture f;
    auto * entry = f.Block();
    auto * demote = f.blocks.Create(f.instructions);
    demote->AppendNewInst(IR::Opcode::DemoteToHelperInvocation, {});
    auto * merge = f.blocks.Create(f.instructions);
    f.If(demote, merge);
    f.syntax_list.push_back({.data = {.block = demote}, .type = Type::Block});
    f.EndIf(merge);
    f.syntax_list.push_back({.data = {.block = merge}, .type = Type::Block});
    f.If(entry, merge);
    f.If(entry, merge);
    f.Block(same_block, true);
    f.syntax_list.push_back({.type = Type::Return});
    f.EndIf(merge);
    f.EndIf(merge);
    f.Block(false, true);
    f.syntax_list.push_back({.type = Type::Return});
    const auto before = f.syntax_list;
    f.DemoteCombinationPass();
    Require(f.syntax_list.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        Require(f.syntax_list[i].type == before[i].type);
        if (before[i].type == Type::Block)
        {
            Require(f.syntax_list[i].data.block == before[i].data.block);
        }
    }
}
void TestUniqueExit(bool has_exit)
{
    Fixture f;
    f.Block();
    auto * demote = f.blocks.Create(f.instructions);
    demote->AppendNewInst(IR::Opcode::DemoteToHelperInvocation, {});
    auto * merge = f.blocks.Create(f.instructions);
    f.If(demote, merge);
    f.syntax_list.push_back({.data = {.block = demote}, .type = Type::Block});
    f.EndIf(merge);
    f.syntax_list.push_back({.data = {.block = merge}, .type = Type::Block});
    auto * exit = f.Block(false, has_exit);
    if (has_exit) f.syntax_list.push_back({.type = Type::Return});
    const auto before = f.syntax_list;
    f.DemoteCombinationPass();
    if (has_exit)
    {
        Require(f.syntax_list[1].type == Type::Block);
        auto it = std::find_if(f.syntax_list.begin(), f.syntax_list.end(), [exit](const Node & n) {
            return n.type == Type::Block && n.data.block == exit;
        });
        Require(it - f.syntax_list.begin() >= 3);
        Require((it - 2)->type == Type::Block && (it - 2)->data.block == demote);
    }
    else
    {
        Require(f.syntax_list.size() == before.size());
        Require(f.syntax_list[1].type == Type::If);
    }
}
int main()
{
    TestNestedExit(false);
    TestNestedExit(true); // Do not stop scanning a block at its demote instruction.
    TestUniqueExit(true);
    TestUniqueExit(false);
}
