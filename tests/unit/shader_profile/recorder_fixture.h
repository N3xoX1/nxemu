#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_common/logging/log.h"
#include "video_settings.h"
namespace Vulkan {
struct GraphicsPipelineCacheKey {
    std::array<u32,104> prefix{};
    FixedPipelineState state{};
};
}
#include "yuzu_video_core/renderer_vulkan/guest_pipeline_recorder.h"
#include <iostream>
VideoSettings videoSettings{};
namespace Common { void SetCurrentThreadName(const char*) {} }
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass,LogLevel,const char*,unsigned,const char*,const char*,const fmt::format_args&) {}
}
void assert_fail_impl(){throw std::runtime_error("renderer assertion");}
[[noreturn]] void unreachable_impl(){throw std::runtime_error("renderer unreachable");}
using namespace Vulkan;
using namespace VideoCommon::ShaderProfile;
#include "legacy_guest_words.inc"
void Check(bool v){if(!v)throw std::runtime_error("recorder replay changed recipe coverage");}

inline std::unique_ptr<Maxwell> RecorderRegisters() {
        auto regs=std::make_unique<Maxwell>();
        regs->gl_front_face=Maxwell::FrontFace::CounterClockWise;regs->gl_cull_face=Maxwell::CullFace::Back;
        regs->polygon_mode_front=Maxwell::PolygonMode::Fill;regs->patch_vertices=3;
        regs->depth_test_func=Maxwell::ComparisonOp::Less_GL;regs->alpha_test_func=Maxwell::ComparisonOp::Always_GL;
        regs->stencil_front_op.func=regs->stencil_back_op.func=Maxwell::ComparisonOp::Always_GL;
        regs->logic_op.op=Maxwell::LogicOp::Op::Copy;regs->rasterize_enable=1;
        regs->blend.color_op=regs->blend.alpha_op=Maxwell::Blend::Equation::Add_GL;
        regs->blend.color_source=regs->blend.alpha_source=Maxwell::Blend::Factor::One_GL;
        regs->blend.color_dest=regs->blend.alpha_dest=Maxwell::Blend::Factor::Zero_GL;
        for(auto& a:regs->vertex_attrib_format){a.constant.Assign(1);a.type.Assign(Maxwell::VertexAttribute::Type::Float);a.size.Assign(Maxwell::VertexAttribute::Size::Size_R32_G32_B32_A32);}
        regs->vertex_attrib_format[0].constant.Assign(0);
    return regs;
}
