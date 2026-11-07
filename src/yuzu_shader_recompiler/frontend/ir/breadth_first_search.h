// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <type_traits>
#include <queue>
#include <unordered_set>

#include <boost/container/small_vector.hpp>

#include "yuzu_shader_recompiler/frontend/ir/value.h"

namespace Shader::IR {

template <typename Pred>
auto BreadthFirstSearch(const Value& value, Pred&& pred)
    -> std::invoke_result_t<Pred, const Inst*> {
    if (value.IsImmediate()) {
        // Nothing to do with immediates
        return std::nullopt;
    }
    // Breadth-first search visiting the right most arguments first
    // Small vector has been determined from shaders in Super Smash Bros. Ultimate
    boost::container::small_vector<const Inst*, 2> visited;
    // Most searches are tiny: avoid a hash allocation until a shader produces
    // enough nodes that repeated linear scans become quadratic.
    std::unique_ptr<std::unordered_set<const Inst*>> large_visited;
    const auto mark_visited = [&](const Inst* inst) {
        if (large_visited) {
            return large_visited->insert(inst).second;
        }
        if (std::ranges::find(visited, inst) != visited.end()) {
            return false;
        }
        visited.push_back(inst);
        if (visited.size() > 64) {
            large_visited = std::make_unique<std::unordered_set<const Inst*>>();
            large_visited->reserve(visited.size() * 2);
            large_visited->insert(visited.begin(), visited.end());
        }
        return true;
    };
    std::queue<const Inst*> queue;
    const Inst* const root = value.InstRecursive();
    mark_visited(root);
    queue.push(root);

    while (!queue.empty()) {
        // Pop one instruction from the queue
        const Inst* const inst{queue.front()};
        queue.pop();
        if (const std::optional result = pred(inst)) {
            // This is the instruction we were looking for
            return result;
        }
        // Visit the right most arguments first
        for (size_t arg = inst->NumArgs(); arg--;) {
            const Value arg_value{inst->Arg(arg)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            // Queue instruction if it hasn't been visited
            const Inst* const arg_inst{arg_value.InstRecursive()};
            if (mark_visited(arg_inst)) {
                queue.push(arg_inst);
            }
        }
    }
    // SSA tree has been traversed and the result hasn't been found
    return std::nullopt;
}

} // namespace Shader::IR
