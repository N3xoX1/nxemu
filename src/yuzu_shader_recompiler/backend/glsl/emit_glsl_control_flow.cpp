// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_shader_recompiler/backend/glsl/emit_glsl_instructions.h"
#include "yuzu_shader_recompiler/backend/glsl/glsl_emit_context.h"
#include "yuzu_shader_recompiler/exception.h"

namespace Shader::Backend::GLSL {

void EmitJoin(EmitContext&) {
    throw NotImplementedException("Join shouldn't be emitted");
}

void EmitDemoteToHelperInvocation(EmitContext& ctx) {
    if (ctx.stage != Stage::Fragment) {
        return;
    }
    ctx.Add("discard;");
}

} // namespace Shader::Backend::GLSL
