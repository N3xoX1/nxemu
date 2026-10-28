// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include "yuzu_video_core/renderer_vulkan/cbuf_size_dependencies.h"
#include "yuzu_common/bit_cast.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/vertex_input_mapping.h"

namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char*, unsigned int, const char*,
                       const char*, const fmt::format_args&) {}
}
VideoSettings videoSettings{};
void assert_fail_impl() { throw std::runtime_error("renderer assertion"); }
[[noreturn]] void unreachable_impl() { throw std::runtime_error("renderer unreachable"); }

using namespace Vulkan;
#include "legacy_fixed_state.inc"
#include "legacy_guest_words.inc"

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static DynamicFeatures Features(unsigned bits) {
    return {bool(bits&1), bool(bits&2), bool(bits&4), bool(bits&8),
            bool(bits&16), bool(bits&32), bool(bits&64)};
}

static void Initialise(Maxwell& regs) {
    regs.gl_front_face = Maxwell::FrontFace::CounterClockWise;
    regs.gl_cull_face = Maxwell::CullFace::Back;
    regs.polygon_mode_front = Maxwell::PolygonMode::Fill;
    regs.patch_vertices = 3;
    regs.depth_test_func = Maxwell::ComparisonOp::Less_GL;
    regs.alpha_test_func = Maxwell::ComparisonOp::Always_GL;
    regs.stencil_front_op.func = Maxwell::ComparisonOp::Always_GL;
    regs.stencil_back_op.func = Maxwell::ComparisonOp::Always_GL;
    regs.logic_op.op = Maxwell::LogicOp::Op::Copy;
    regs.rasterize_enable = 1;
    regs.blend.color_op = Maxwell::Blend::Equation::Add_GL;
    regs.blend.alpha_op = Maxwell::Blend::Equation::Add_GL;
    regs.blend.color_source = Maxwell::Blend::Factor::One_GL;
    regs.blend.alpha_source = Maxwell::Blend::Factor::One_GL;
    regs.blend.color_dest = Maxwell::Blend::Factor::Zero_GL;
    regs.blend.alpha_dest = Maxwell::Blend::Factor::Zero_GL;
    for (auto& blend : regs.blend_per_target) {
        blend.color_op = regs.blend.color_op;
        blend.alpha_op = regs.blend.alpha_op;
        blend.color_source = regs.blend.color_source;
        blend.alpha_source = regs.blend.alpha_source;
        blend.color_dest = regs.blend.color_dest;
        blend.alpha_dest = regs.blend.alpha_dest;
    }
    for (size_t i=0; i<regs.vertex_attrib_format.size(); ++i) {
        auto& a=regs.vertex_attrib_format[i];
        a.constant.Assign(i >= 4 ? 1U : 0U);
        a.buffer.Assign(static_cast<u32>(i%4));
        a.type.Assign(Maxwell::VertexAttribute::Type::Float);
        a.size.Assign(Maxwell::VertexAttribute::Size::Size_R32_G32_B32_A32);
    }
}

int main() try {
    // First HLE translation narrows conservative CBUF sizes. Its normalized
    // alias must retain replacement-free eligibility for a later plain draw.
    {
        std::unordered_map<int, std::shared_ptr<int>> cache;
        std::unordered_set<int> unreplaced;
        const auto pipeline = std::make_shared<int>(1);
        cache.emplace(10, pipeline);
        unreplaced.insert(10);
        Check(PublishNormalizedPipelineAlias(10, 11, pipeline, cache, unreplaced),
              "normalized HLE pipeline alias missing");
        Check(cache.at(11) == pipeline && unreplaced.contains(11),
              "normalized alias lost replacement-free HLE eligibility");
        Check(FindUnreplacedPipelineAlias(11, cache, unreplaced) == pipeline,
              "plain draw would queue a new build instead of sharing HLE twin");
        Check(!PublishNormalizedPipelineAlias(10, 11, pipeline, cache, unreplaced),
              "existing alias republished");
        // Replacement-bearing shaders must keep their own native combinations.
        cache.emplace(20, pipeline);
        Check(PublishNormalizedPipelineAlias(20, 21, pipeline, cache, unreplaced) &&
                  !unreplaced.contains(21), "replacement-bearing alias became reusable");
        Check(!FindUnreplacedPipelineAlias(21, cache, unreplaced),
              "replacement-bearing twin reused before translation");
        const auto other = std::make_shared<int>(2);
        cache.emplace(31, other);
        Check(!PublishNormalizedPipelineAlias(10, 31, pipeline, cache, unreplaced) &&
                  cache.at(31) == other && !unreplaced.contains(31),
              "conflicting pipeline inherited unrelated HLE eligibility");
        Check(!PublishNormalizedPipelineAlias(10, 10, pipeline, cache, unreplaced),
              "identity alias published");
        cache.emplace(40, nullptr);unreplaced.insert(40);unreplaced.insert(41);
        Check(!FindUnreplacedPipelineAlias(40, cache, unreplaced) &&
                  !FindUnreplacedPipelineAlias(41, cache, unreplaced),
              "missing or failed HLE pipeline reused");
    }
    auto tracking = std::make_unique<Tegra::Engines::Maxwell3D::DirtyState>();
    GuestPipelineState::Track(*tracking);
    const auto tracked = [&](size_t byte) { return tracking->shader_profile_registers[byte / 4] != 0; };
    Check(tracked(offsetof(Maxwell, vertex_streams)) && tracked(offsetof(Maxwell, blend)+offsetof(Maxwell::Blend,color_op)), "Portable state changes are untracked");
    Check(!tracked(offsetof(Maxwell,blend)+offsetof(Maxwell::Blend,separate_alpha)) &&
          !tracked(offsetof(Maxwell,blend)+offsetof(Maxwell::Blend,enable_global_color_key)) &&
          !tracked(offsetof(Maxwell,blend_per_target)+offsetof(Maxwell::BlendPerTarget,separate_alpha)),
          "unused blend controls trigger capture");
    Check(tracked(offsetof(Maxwell, vertex_streams) + offsetof(Maxwell::VertexStream, frequency)), "Vertex divisor change untracked");
    Check(!tracked(offsetof(Maxwell, vertex_streams) + offsetof(Maxwell::VertexStream, address_low)), "Vertex address triggers recipe capture");
    Check(!tracked(offsetof(Maxwell, rt)), "Render target address triggers recipe capture");
    Check(tracked(offsetof(Maxwell, viewport_transform) + offsetof(std::remove_reference_t<decltype(Maxwell{}.viewport_transform[0])>, swizzle)), "Viewport swizzle untracked");
    // Disabled vertex metadata cannot affect guest recipes or projected keys.
    // Exercise all host classes, HLE/plain, and full-size transform-feedback keys.
    {
        auto regs=std::make_unique<Maxwell>();
        struct AllLoads { bool Generic(size_t) const { return true; } } loads;
        for(bool hle:{false,true})for(bool xfb:{false,true}) {
            Initialise(*regs);regs->transform_feedback_enabled=xfb;
            const auto hint=hle?Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro:Tegra::Engines::Maxwell3D::EngineHint::None;
            const auto capture=[&]{return GuestPipelineState::Capture(*regs,Maxwell::PrimitiveTopology::Triangles,hint);};
            const auto base=capture();
            for(size_t i=4;i<Maxwell::NumVertexAttributes;++i) {
                auto& attr=regs->vertex_attrib_format[i];
                attr.buffer.Assign(static_cast<u32>((i*17)%32));attr.offset.Assign(static_cast<u32>(i*37));
                attr.type.Assign(Maxwell::VertexAttribute::Type::UInt);
                attr.size.Assign(Maxwell::VertexAttribute::Size::Size_R32);
            }
            // A disabled-only stream must not introduce an instancing divisor.
            regs->vertex_stream_instances.is_instanced[31]=1;regs->vertex_streams[31].frequency=31;
            const auto disabled=capture();
            Check(base.Words()==disabled.Words(),"disabled vertex metadata leaked into guest words");
            const auto decoded=GuestPipelineState::FromWords(disabled.Words());
            Check(decoded.has_value(),"new canonical vertex recipe rejected");
            for(unsigned bits=0;bits<128;++bits) {
                const auto a=base.Project(Features(bits)),b=disabled.Project(Features(bits));
                Check(a==b && a.Hash()==b.Hash() && a.Size()==b.Size() &&
                      std::memcmp(&a,&b,a.Size())==0,"disabled vertex metadata split a host key");
                Check(decoded->Project(Features(bits))==a,"canonical vertex recipe round-trip split key");
                Check(a.enabled_divisors==b.enabled_divisors && a.binding_divisors==b.binding_divisors,
                      "disabled vertex metadata affected instancing");
                if(!(bits&32)) {
                    const auto ma=MakeVertexInputMapping(a,loads),mb=MakeVertexInputMapping(b,loads);
                    Check(ma.locations==mb.locations && ma.bindings==mb.bindings &&
                          ma.num_attributes==mb.num_attributes && ma.num_bindings==mb.num_bindings,
                          "disabled vertex metadata changed compact input mapping");
                }
            }
            const auto original=regs->vertex_attrib_format[0];
            for(int field=0;field<4;++field) {
                auto& attr=regs->vertex_attrib_format[0];attr=original;
                if(field==0)attr.buffer.Assign(2);
                if(field==1)attr.offset.Assign(100);
                if(field==2)attr.type.Assign(Maxwell::VertexAttribute::Type::UInt);
                if(field==3)attr.size.Assign(Maxwell::VertexAttribute::Size::Size_R32);
                const auto enabled=capture();
                Check(enabled.Words()!=disabled.Words(),"active vertex metadata merged in guest words");
                for(unsigned bits=0;bits<128;++bits) {
                    const auto a=disabled.Project(Features(bits)),b=enabled.Project(Features(bits));
                    if(!(bits&32) || field==2)Check(a!=b,"active vertex layout/type merged in host key");
                }
            }
            regs->vertex_attrib_format[0]=original;
            regs->vertex_attrib_format[4].constant.Assign(0);
            const auto enabled=capture();
            for(unsigned bits=0;bits<128;++bits)
                Check(enabled.Project(Features(bits))!=disabled.Project(Features(bits)),"enabled and disabled attribute merged");
        }
    }
    {
        auto regs=std::make_unique<Maxwell>();Initialise(*regs);
        std::set<std::string> previous,current;
        for(unsigned i=0;i<256;++i) {
            regs->vertex_attrib_format[4].offset.Assign(i*4);
            regs->vertex_attrib_format[5].buffer.Assign(i%32);
            FixedPipelineState state{};LegacyState legacy{};
            FixedPipelineState::RefreshFlags a{true,true,true,true},b=a;
            state.RefreshFromRegisters(*regs,Maxwell::PrimitiveTopology::Triangles,
                Tegra::Engines::Maxwell3D::EngineHint::None,Features(67),a);
            legacy.RefreshReference(*regs,Maxwell::PrimitiveTopology::Triangles,
                Tegra::Engines::Maxwell3D::EngineHint::None,Features(67),b);
            previous.emplace(reinterpret_cast<const char*>(&legacy),state.Size());
            current.emplace(reinterpret_cast<const char*>(&state),state.Size());
        }
        Check(previous.size()==256 && current.size()==1,"disabled vertex layout corpus did not deduplicate");
    }
    size_t checks{};
    for (unsigned bits=0; bits<128; ++bits) {
        auto regs=std::make_unique<Maxwell>();
        Initialise(*regs);
        const auto features=Features(bits);
        FixedPipelineState local{};
        LegacyState reference{};
        for (unsigned step=0; step<64; ++step) {
            // Transition history exercises stale fields and dirty-bit side effects.
            const bool dirty_input=step%3==0 || step==0;
            const bool dirty_blend=step%4==0 || step==0;
            const bool dirty_viewport=step%5==0 || step==0;
            if (dirty_input) {
                regs->vertex_attrib_format[0].offset.Assign(step*4);
                regs->vertex_attrib_format[1].type.Assign(
                    static_cast<Maxwell::VertexAttribute::Type>(1+step%7));
                regs->vertex_stream_instances.is_instanced[0]=step%2;
                regs->vertex_streams[0].frequency=step%3;
            }
            if (dirty_blend) {
                regs->blend.enable[0]=1;
                regs->blend.color_source = step%8==0 ? Maxwell::Blend::Factor::Source1Color_GL
                                                     : Maxwell::Blend::Factor::One_GL;
                regs->color_mask[0].R.Assign(step%2);
                regs->blend.color_op=static_cast<Maxwell::Blend::Equation>(1+step%5);
            }
            if (dirty_viewport) regs->viewport_transform[0].swizzle.raw=step;
            regs->vertex_streams[0].enable.Assign(step%2);
            regs->rt[0].format = step%6==0 ? Tegra::RenderTargetFormat::NONE
                                          : Tegra::RenderTargetFormat::A8B8G8R8_UNORM;
            regs->vertex_streams[0].stride.Assign(step*4);
            regs->window_origin.flip_y.Assign(step%2);
            regs->depth_write_enabled=step%2;
            regs->zeta_enable=step%3==0;
            regs->zeta.format=static_cast<Tegra::DepthFormat>(20+step%2);
            regs->gl_cull_test_enabled=step%2;
            regs->logic_op.enable=step%2;
            regs->logic_op.op=static_cast<Maxwell::LogicOp::Op>(0x1500+step%16);
            regs->transform_feedback_enabled=step%7==0;
            regs->transform_feedback.controls[0].stride=step*4;
            regs->stream_out_layout[0][0].attribute0.Assign(step);
            const auto topology=static_cast<Maxwell::PrimitiveTopology>(step%15);
            const auto hint=step%2 ? Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro : Tegra::Engines::Maxwell3D::EngineHint::None;
            FixedPipelineState::RefreshFlags a{dirty_input, dirty_viewport, dirty_blend, dirty_blend};
            auto b=a;
            local.RefreshFromRegisters(*regs, topology, hint, features, a);
            reference.RefreshReference(*regs, topology, hint, features, b);
            // Deliberate change from the frozen implementation: inactive depth
            // formats do not affect the native render pass or translated shaders.
            if (reference.depth_enabled == 0) reference.depth_format.Assign(0);
            // Deliberate canonicalization: disabled input metadata has no consumer.
            for(auto& attr:reference.attributes)if(!attr.enabled)attr.raw=0;
            Check(std::memcmp(&local, &reference, sizeof(local))==0, "refactor differs from frozen legacy state");
            Check(a.vertex_input==b.vertex_input && a.viewport_swizzles==b.viewport_swizzles &&
                  a.blending==b.blending && a.blend_enable==b.blend_enable, "dirty-bit transitions differ");
            const auto guest=GuestPipelineState::Capture(*regs, topology, hint);
            const auto projected=guest.Project(features);
            const auto decoded=GuestPipelineState::FromWords(guest.Words());
            Check(decoded.has_value() && decoded->Words()==guest.Words() &&
                  decoded->Project(features)==projected, "recipe round-trip changed state");
            std::array<u32, GuestPipelineState::WordCount+2> guarded{};
            guarded.front() = 0x12345678; guarded.back() = 0x87654321;
            guest.WriteWords(std::span<u32,GuestPipelineState::WordCount>(guarded.data()+1,GuestPipelineState::WordCount));
            const auto legacy_words=LegacyWords(guest);
            Check(guarded.front()==0x12345678 && guarded.back()==0x87654321 &&
                  std::equal(legacy_words.begin(),legacy_words.end(),guarded.begin()+1),
                  "allocation-free writer changed legacy words or bounds");
            if (!(local==projected)) {
                std::cerr << "projection mismatch features=" << bits << " step=" << step << '\n';
                const auto* x=reinterpret_cast<const unsigned char*>(&local);
                const auto* y=reinterpret_cast<const unsigned char*>(&projected);
                for (size_t i=0;i<local.Size();++i) if(x[i]!=y[i]) { std::cerr << "byte " << i << " legacy=" << unsigned(x[i]) << " projected=" << unsigned(y[i]) << '\n'; break; }
                throw std::runtime_error("guest projection differs from legacy effective key");
            }
            Check((guest.complete.raw1&63)==0 && guest.complete.vertex_stride_workaround==0,
                  "guest state contains host options");
            ++checks;
        }
    }
    auto regs=std::make_unique<Maxwell>(); Initialise(*regs);
    for (unsigned bits=0; bits<128; ++bits) {
        regs->zeta_enable=0;
        regs->zeta.format=static_cast<Tegra::DepthFormat>(20);
        auto disabled=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles,
            Tegra::Engines::Maxwell3D::EngineHint::None);
        auto old_words=disabled.Words();
        old_words[2] |= 20U << 6; // Existing v2 profiles retain this inactive field.
        const auto legacy=GuestPipelineState::FromWords(old_words);
        Check(legacy.has_value(), "existing recipe with inactive depth format rejected");
        Check(legacy->Project(Features(bits))==disabled.Project(Features(bits)),
              "existing recipe retained irrelevant depth format");
        FixedPipelineState old_cache=legacy->complete;
        old_cache.NormalizeInactiveFields();
        Check(old_cache.depth_format==0, "legacy local cache format was not normalized");
        regs->zeta_enable=1;
        auto enabled=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles,
            Tegra::Engines::Maxwell3D::EngineHint::None);
        regs->zeta.format=static_cast<Tegra::DepthFormat>(21);
        auto different=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles,
            Tegra::Engines::Maxwell3D::EngineHint::None);
        Check(enabled.Project(Features(bits))!=different.Project(Features(bits)),
              "active depth format variants were merged");
    }
    regs->zeta_enable=0;
    regs->blend.enable[0]=1;
    auto first=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles, Tegra::Engines::Maxwell3D::EngineHint::None);
    regs->vertex_attrib_format[0].offset.Assign(64);
    auto layout=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles, Tegra::Engines::Maxwell3D::EngineHint::None);
    Check(first.Words()!=layout.Words(), "vertex layout combinations merged");
    Check(first.Project(Features(63))==layout.Project(Features(63)), "test requires same dynamic host key");
    Check(first.Project(Features(0))!=layout.Project(Features(0)), "static target lost vertex layout combination");
    regs->color_mask[0].R.Assign(1);
    auto blend=GuestPipelineState::Capture(*regs, Maxwell::PrimitiveTopology::Triangles, Tegra::Engines::Maxwell3D::EngineHint::None);
    Check(blend.Words()!=layout.Words(), "blend combinations merged");
    Check(blend.Project(Features(63))==layout.Project(Features(63)), "dynamic blend should share local key");
    Check(blend.Project(Features(0))!=layout.Project(Features(0)), "static target lost blend combination");
    auto invalid=blend.Words(); invalid[0]=999;
    Check(!GuestPipelineState::FromWords(invalid), "unknown recipe schema accepted");
    invalid=blend.Words(); invalid[1]|=1;
    Check(!GuestPipelineState::FromWords(invalid), "host-dependent recipe accepted");
    invalid=blend.Words(); invalid.pop_back();
    Check(!GuestPipelineState::FromWords(invalid), "truncated recipe accepted");
    invalid=blend.Words(); invalid[4]=256;
    Check(!GuestPipelineState::FromWords(invalid), "out-of-range narrow field accepted");
    std::cout << "PASS: " << checks << " projections, full legacy-state equality, dirty-bit equality, hidden vertex/blend combinations preserved\n";
    return 0;
} catch(const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
}
