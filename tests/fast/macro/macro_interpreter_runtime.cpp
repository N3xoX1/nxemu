// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cassert>
#include <cstdint>
#include <vector>

// Compile the unmodified production interpreter against a small fake Maxwell3D backend. This
// validates instruction and parameter lifetimes without requiring Vulkan or a game image.
#include "yuzu_video_core/macro/macro_interpreter.cpp"

namespace Tegra {
// Satisfy the base engine's constructor, destructor and RTTI without pulling in the GPU renderer.
class HLEMacro {};
MacroEngine::MacroEngine(Engines::Maxwell3D& engine) : maxwell3d{engine} {}
MacroEngine::~MacroEngine() = default;
} // namespace Tegra

namespace {
using Tegra::Macro::Opcode;

Opcode MakeAddImmediate(Tegra::Macro::ResultOperation result, std::uint32_t dst,
                        std::uint32_t src, std::int32_t immediate, bool exit = false) {
    Opcode opcode{};
    opcode.operation.Assign(Tegra::Macro::Operation::AddImmediate);
    opcode.result_operation.Assign(result);
    opcode.dst.Assign(dst);
    opcode.src_a.Assign(src);
    opcode.immediate.Assign(immediate);
    opcode.is_exit.Assign(exit ? 1 : 0);
    return opcode;
}
} // namespace

int main() {
    using Tegra::Macro::ResultOperation;

    for (const auto target : {0x100U, 0xE00U}) {
        Tegra::Engines::Maxwell3D maxwell;
        std::vector<u32> params;
        maxwell.current_macro_params = &params;

        // Set address; send arg0; fetch arg1 after Send; send the captured arg1; exit + delay.
        const std::vector<u32> code{
            MakeAddImmediate(ResultOperation::MoveAndSetMethod, 2, 0, target).raw,
            MakeAddImmediate(ResultOperation::MoveAndSend, 2, 1, 0).raw,
            MakeAddImmediate(ResultOperation::FetchAndSend, 3, 0, 0).raw,
            MakeAddImmediate(ResultOperation::MoveAndSend, 4, 3, 0, true).raw,
            MakeAddImmediate(ResultOperation::Move, 5, 0, 0).raw,
        };
        Tegra::MacroInterpreterImpl interpreter{maxwell, code};
        for (u32 iteration = 0; iteration < 3; ++iteration) {
            // Reuse the cached interpreter: no parameter pointers or snapshots may leak into
            // the next execution, even when a previous Send invalidated the input vector.
            params = {0xAAU + iteration, 0xBBU + iteration};
            maxwell.calls.clear();
            maxwell.mutate_on_macro_send = target >= 0xE00;
            interpreter.Execute(params, 0);

            assert(maxwell.calls.size() == 3);
            assert(maxwell.calls[0].first == target);
            assert(maxwell.calls[0].second == 0xAAU + iteration);
            assert(maxwell.calls[1].second == 0U);
            assert(maxwell.calls[2].second == 0xBBU + iteration);
            if (target >= 0xE00) {
                assert(params.size() == 4096);
            } else {
                assert(params.size() == 2);
            }
        }
    }
}
