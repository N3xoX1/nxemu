// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <iterator>
#include <list>
#include <memory>
#include <stdexcept>
#include <vector>
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
using namespace Shader::Maxwell::Flow;
using Shader::LogicError;
using Shader::Maxwell::Location;

void Check(bool ok) {
    if (!ok) throw std::runtime_error("control-flow lookup regression");
}
struct Statement;
using Tree = std::list<Statement>;
using Node = Tree::iterator;
struct Statement { Statement* up{}; Tree children; };
#include "ordered.inc"

struct Fixture {
    struct Function { boost::intrusive::set<Block> blocks; };
    std::array<Function, 1> functions;
    unsigned splits{};
    Block* last_visited{};
    bool InspectVisitedBlocks(FunctionId function_id, const Label& label);
    void Split(Block* old_block, Block* new_block, Location pc) {
        ++splits;
        last_visited = old_block;
        new_block->begin = pc;
        new_block->end = old_block->end;
        old_block->end = pc;
    }
};
#include "visited_blocks.inc"

void TestSiblingOrder() {
    for (size_t count : {1U, 2U, 3U, 32U, 129U, 257U}) {
        Statement parent;
        std::vector<Node> nodes;
        for (size_t i = 0; i < count; ++i) {
            parent.children.emplace_back();
            parent.children.back().up = &parent;
            nodes.push_back(std::prev(parent.children.end()));
        }
        for (size_t left = 0; left < count; ++left) {
            for (size_t right = 0; right < count; ++right) {
                Check(AreOrdered(nodes[left], nodes[right]) == (left < right));
            }
        }
    }
}
void TestVisitedBlocks() {
    for (u32 address = 8; address < 256; address += 8) {
        // Rebuild after every query: the real method splits a matched interval.
        std::array<Block, 3> blocks;
        Fixture fixture;
        for (size_t i = 0; i < blocks.size(); ++i) {
            blocks[i].begin = Location{static_cast<u32>(40 + i * 64)};
            blocks[i].end = Location{static_cast<u32>(64 + i * 64)};
            fixture.functions[0].blocks.insert(blocks[i]);
        }
        const Location pc{address};
        const auto expected = std::find_if(blocks.begin(), blocks.end(), [pc](const Block& block) {
            return block.Contains(pc);
        });
        Block split;
        const Label label{.address = pc, .block = &split};
        bool dangling{};
        bool found{};
        try { found = fixture.InspectVisitedBlocks(0, label); }
        catch (const LogicError&) { dangling = true; }
        const bool matches = expected != blocks.end();
        Check(dangling == (matches && expected->begin == pc));
        Check(found == (matches && !dangling));
        Check(fixture.splits == (found ? 1U : 0U));
        if (found) Check(fixture.last_visited == &*expected && split.begin == pc);
        fixture.functions[0].blocks.clear();
    }
    Fixture empty;
    Block block;
    Check(!empty.InspectVisitedBlocks(0, Label{.address = Location{8}, .block = &block}));
}
int main() {
    TestSiblingOrder();
    TestVisitedBlocks();
}
