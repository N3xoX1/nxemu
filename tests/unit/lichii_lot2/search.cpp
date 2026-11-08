// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "yuzu_shader_recompiler/frontend/ir/breadth_first_search.h"

namespace IR = Shader::IR;
using Result = std::optional<const IR::Inst*>;
void Check(bool ok) {
    if (!ok) throw std::runtime_error("shader graph search regression");
}
// Reference traversal before the optimization, used as an order/result oracle.
template <class Pred> Result LinearSearch(const IR::Value& value, Pred&& pred) {
    if (value.IsImmediate()) return std::nullopt;
    boost::container::small_vector<const IR::Inst*, 2> visited;
    std::queue<const IR::Inst*> queue;
    queue.push(value.InstRecursive());
    while (!queue.empty()) {
        const auto* inst = queue.front();
        queue.pop();
        if (const auto result = pred(inst)) return result;
        for (size_t arg = inst->NumArgs(); arg--;) {
            const auto value = inst->Arg(arg);
            if (value.IsImmediate()) continue;
            const auto* child = value.InstRecursive();
            if (std::find(visited.begin(), visited.end(), child) == visited.end()) {
                visited.push_back(child);
                queue.push(child);
            }
        }
    }
    return std::nullopt;
}
struct Graph {
    std::vector<std::unique_ptr<IR::Inst>> nodes;
    IR::Value Add(IR::Value a = IR::Value{0U}, IR::Value b = IR::Value{0U}) {
        auto node = std::make_unique<IR::Inst>(IR::Opcode::IAdd32, 0U);
        node->SetArg(0, a);
        node->SetArg(1, b);
        auto* result = node.get();
        nodes.push_back(std::move(node));
        return IR::Value{result};
    }
    IR::Value Chain(size_t count) {
        IR::Value previous{0U}, second{1U};
        for (size_t i = 0; i < count; ++i) {
            const auto next = Add(second, previous);
            second = previous;
            previous = next;
        }
        return previous;
    }
};
void Test() {
    Check(!IR::BreadthFirstSearch(IR::Value{1U}, [](const IR::Inst*) -> Result {
        throw std::runtime_error("predicate called for an immediate");
    }));
    Graph graph;
    const auto left = graph.Add(), right = graph.Add();
    const auto root = graph.Add(left, right);
    const auto first = IR::BreadthFirstSearch(root, [&](const IR::Inst* inst) -> Result {
        return inst == root.Inst() ? std::nullopt : Result{inst};
    });
    Check(first == right.Inst()); // Rightmost argument has priority.
    auto identity = std::make_unique<IR::Inst>(IR::Opcode::Identity, 0U);
    identity->SetArg(0, root);
    Check(IR::BreadthFirstSearch(IR::Value{identity.get()}, [](const IR::Inst* i) {
        return Result{i};
    }) == root.Inst());
    std::mt19937 random{917};
    for (size_t count : {1U, 2U, 63U, 64U, 65U, 66U, 128U, 1024U}) {
        Graph dag;
        const auto entry = dag.Chain(count);
        for (unsigned trial = 0; trial < 10; ++trial) {
            const auto* target = trial == 0 ? nullptr : dag.nodes[random() % count].get();
            std::vector<const IR::Inst*> old_order, new_order;
            const auto before = LinearSearch(entry, [&](const IR::Inst* i) -> Result {
                old_order.push_back(i);
                return i == target ? Result{i} : std::nullopt;
            });
            const auto after = IR::BreadthFirstSearch(entry, [&](const IR::Inst* i) -> Result {
                new_order.push_back(i);
                return i == target ? Result{i} : std::nullopt;
            });
            Check(before == after && old_order == new_order);
        }
    }
    // A cyclic Phi must visit the root once, rather than enqueueing it again.
    auto phi = std::make_unique<IR::Inst>(IR::Opcode::Phi, static_cast<u32>(IR::Type::U32));
    phi->AddPhiOperand(nullptr, IR::Value{phi.get()});
    phi->AddPhiOperand(nullptr, left);
    unsigned visits{};
    Check(!IR::BreadthFirstSearch(IR::Value{phi.get()}, [&](const IR::Inst*) -> Result {
        ++visits;
        return std::nullopt;
    }));
    Check(visits == 2);
}
void Benchmark() {
    size_t checksum{};
    for (size_t count : {1U, 8U, 64U, 512U, 4096U, 20000U}) {
        Graph graph;
        const auto root = graph.Chain(count);
        const size_t repeats = count <= 64 ? 10000 : count <= 512 ? 100 : 3;
        const auto measure = [&](bool optimized) {
            std::vector<double> samples;
            for (unsigned round = 0; round < 5; ++round) {
                size_t visited{};
                const auto pred = [&](const IR::Inst*) -> Result {
                    ++visited;
                    return std::nullopt;
                };
                const auto begin = std::chrono::steady_clock::now();
                for (size_t i = 0; i < repeats; ++i) {
                    if (optimized) IR::BreadthFirstSearch(root, pred);
                    else LinearSearch(root, pred);
                }
                const auto end = std::chrono::steady_clock::now();
                checksum += visited;
                samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count() /
                                  static_cast<double>(repeats));
            }
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };
        const auto old_time = measure(false), new_time = measure(true);
        std::printf("BFS nodes=%zu baseline_us=%.3f optimized_us=%.3f speedup=%.2fx\n",
                    count, old_time, new_time, old_time / new_time);
    }
    std::printf("checksum=%zu\n", checksum);
}
int main(int argc, char** argv) {
    Test();
    if (argc > 1 && std::string_view{argv[1]} == "--benchmark") Benchmark();
}
