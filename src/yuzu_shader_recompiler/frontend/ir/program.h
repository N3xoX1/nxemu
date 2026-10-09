// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "yuzu_shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/program_header.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_shader_recompiler/stage.h"

namespace Shader::IR {

struct Program {
    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    Info info;
    Stage stage{};
    std::array<u32, 3> workgroup_size{};
    OutputTopology output_topology{};
    u32 output_vertices{};
    u32 invocations{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    bool shared_memory_16_to_12_packing{};
    bool is_geometry_passthrough{};
};

[[nodiscard]] std::string DumpProgram(const Program& program);

} // namespace Shader::IR
