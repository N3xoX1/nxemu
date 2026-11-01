// SPDX-License-Identifier: GPL-2.0-or-later
// Real register layouts, format conversion and unchanged production method bodies.
// Only command recording and the host device are replaced; no GPU timing is claimed.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <vulkan/vulkan.h>
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vertex_input_mapping.h"

using Vulkan::VertexInputMapping;
using boost::container::static_vector;
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
namespace Dirty = Vulkan::Dirty;
using Bindings = boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32>;
using Attributes = boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32>;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Device {
    bool dynamic{true};
    u32 attributes{32}, bindings{32};
    bool depth_bounds{true};
    bool IsExtVertexInputDynamicStateSupported() const { return dynamic; }
    u32 GetMaxVertexInputAttributes() const { return attributes; }
    u32 GetMaxVertexInputBindings() const { return bindings; }
    bool MustEmulateScaledFormats() const { return false; }
    bool CanUseDepthBoundsDynamicState() const { return depth_bounds; }
    VkFormat GetSupportedFormat(VkFormat format, u32, int) const { return format; }
};
namespace MaxwellToVK {
enum FormatType { Buffer };
// Invalid formats must fail, even in Release builds.
#undef ASSERT_MSG
#define ASSERT_MSG(condition, ...) Require(condition, "invalid format")
#undef UNIMPLEMENTED_MSG
#define UNIMPLEMENTED_MSG(...) Require(false, "unimplemented format")
#include "format.inc"
}
struct BufferCacheRuntime {
    bool track_vertex_input_strides{true};
    u32 known_vertex_input_strides{};
    std::array<u32, 32> vertex_input_strides{};
    void NotifyVertexBufferStride(u32 index, u32 stride,
                                 Tegra::Engines::Maxwell3D::DirtyState::Flags& dirty) noexcept;
#ifdef VERTEX_INPUT_COMPARE
    bool needs_vertex_input_refresh{};
    void NotifySuppliedVertexBufferStride(u32 index, u32 stride) noexcept;
#include "consume_supplied.inc"
#endif
};
#include "notify.inc"
#ifdef VERTEX_INPUT_COMPARE
#include "notify_supplied.inc"
#endif
struct Capture {
    Bindings bindings;
    Attributes attributes;
    u64 commands{};
};
namespace vk {
struct CommandBuffer {
    Capture* capture;
    void SetVertexInputEXT(const Bindings& bindings, const Attributes& attributes) {
        capture->bindings = bindings;
        capture->attributes = attributes;
        ++capture->commands;
    }
};
}
struct Scheduler {
    Capture capture;
    template<class F>
#ifdef _MSC_VER
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    void Record(F callback) { callback(vk::CommandBuffer{&capture}); }
};
struct Engine {
    Maxwell regs{};
    Tegra::Engines::Maxwell3D::DirtyState dirty{};
};
struct RasterizerVulkan {
    Device device;
    Engine engine;
    Engine* maxwell3d{&engine};
    BufferCacheRuntime buffer_cache_runtime;
    Scheduler scheduler;
    void UpdateVertexInput(Maxwell& regs);
    void RefreshVertexInput(Maxwell& regs);
#ifdef VERTEX_INPUT_COMPARE
    void Original(Maxwell& regs);
    void Supplied(Maxwell& regs);
#endif
};
#include "update.inc"
#ifdef VERTEX_INPUT_COMPARE
#include "original.inc"
#include "supplied.inc"
#endif

void Layout(RasterizerVulkan& f, u32 count, bool shared = false) {
    for (u32 i = 0; i < 32; ++i) {
        auto& a = f.engine.regs.vertex_attrib_format[i];
        a = {};
        a.constant.Assign(i >= count);
        a.buffer.Assign(shared ? 31 : i);
        a.type.Assign(Maxwell::VertexAttribute::Type::Float);
        a.size.Assign(Maxwell::VertexAttribute::Size::Size_R32_G32_B32_A32);
        a.offset.Assign(i * 16);
        f.engine.regs.vertex_streams[i].stride.Assign(64);
        f.engine.regs.vertex_streams[i].frequency = 3;
    }
    f.engine.dirty.flags.set(Dirty::VertexInput);
}

using Update = void (RasterizerVulkan::*)(Maxwell&);
void Notify(RasterizerVulkan& f, Update update, u32 index, u32 stride) {
#ifdef VERTEX_INPUT_COMPARE
    if (update == &RasterizerVulkan::Original) {
        return;
    }
    if (update != &RasterizerVulkan::UpdateVertexInput) {
        f.buffer_cache_runtime.NotifySuppliedVertexBufferStride(index, stride);
        return;
    }
#endif
    f.buffer_cache_runtime.NotifyVertexBufferStride(index, stride, f.engine.dirty.flags);
}
void Check(Update update) {
    auto owned = std::make_unique<RasterizerVulkan>();
    auto& f = *owned;
    auto draw = [&] { (f.*update)(f.engine.regs); };
    Layout(f, 32);
    f.engine.dirty.flags.set();
    draw();
    Require(f.scheduler.capture.attributes.size() == 32, "initial state includes attribute 31");
    for (u32 i = 0; i < 32; ++i) {
        Require(!f.engine.dirty.flags[Dirty::VertexAttribute0 + i] &&
                !f.engine.dirty.flags[Dirty::VertexBinding0 + i], "slot dirty flags cleared");
    }
    Require(f.engine.dirty.flags[Dirty::Scissors], "unrelated flags preserved");
    const auto commands = f.scheduler.capture.commands;
    draw();
    Require(f.scheduler.capture.commands == commands, "clean draw records nothing");
    f.engine.regs.vertex_attrib_format[0].offset.Assign(12);
    f.engine.dirty.flags.set(Dirty::VertexInput);
    f.engine.dirty.flags.set(Dirty::VertexAttribute0);
    draw();
    Require(f.scheduler.capture.attributes.size() == 32 &&
            f.scheduler.capture.attributes[0].offset == 12, "attribute 0 update preserves full state");
    f.engine.regs.vertex_attrib_format[31].offset.Assign(20);
    f.engine.dirty.flags.set(Dirty::VertexInput);
    f.engine.dirty.flags.set(Dirty::VertexAttribute31);
    draw();
    Require(f.scheduler.capture.attributes.back().offset == 20, "highest attribute updated");
    f.engine.regs.vertex_stream_instances.is_instanced[31] = 1;
    f.engine.regs.vertex_streams[31].frequency = 7;
    f.engine.dirty.flags.set(Dirty::VertexInput);
    f.engine.dirty.flags.set(Dirty::VertexBinding31);
    draw();
    Require(f.scheduler.capture.attributes.size() == 32 &&
            f.scheduler.capture.bindings.back().divisor == 7 &&
            f.scheduler.capture.bindings.back().inputRate == VK_VERTEX_INPUT_RATE_INSTANCE,
            "binding-only update preserves attributes and updates divisor");
    f.engine.regs.vertex_streams[31].stride.Assign(80);
    Notify(f, update, 31, 80);
    draw();
    Require(f.scheduler.capture.bindings.back().stride == 80, "stride-only refresh");
    Layout(f, 4, true);
    draw();
    Require(f.scheduler.capture.attributes.size() == 4 &&
            f.scheduler.capture.bindings.size() == 1 &&
            f.scheduler.capture.bindings[0].binding == 31, "shared sparse binding 31");
    Layout(f, 0);
    draw();
    Require(f.scheduler.capture.attributes.empty() && f.scheduler.capture.bindings.empty(),
            "all-constant layout removes previous state");
    // Invalidations (new command buffer / channel) must rebuild without per-slot dirtiness.
    Layout(f, 8);
    draw();
    f.engine.dirty.flags.set(Dirty::VertexInput);
    draw();
    Require(f.scheduler.capture.attributes.size() == 8, "command buffer invalidation");
    Layout(f, 2);
    draw();
    Require(f.scheduler.capture.attributes.size() == 2, "new channel layout");
}

void StridesAndPolicy() {
    BufferCacheRuntime runtime;
    Tegra::Engines::Maxwell3D::DirtyState::Flags dirty;
    runtime.NotifyVertexBufferStride(31, 0, dirty);
    Require(dirty[Dirty::VertexInput], "initial zero stride is known");
    dirty.reset(Dirty::VertexInput);
    runtime.NotifyVertexBufferStride(31, 0, dirty);
    Require(!dirty[Dirty::VertexInput], "rebasing unchanged stride records nothing");
    runtime.NotifyVertexBufferStride(31, 64, dirty);
    Require(dirty[Dirty::VertexInput], "stride change invalidates vertex input");
    dirty.reset(Dirty::VertexInput);
    runtime.track_vertex_input_strides = false;
    runtime.NotifyVertexBufferStride(0, 32, dirty);
    Require(!dirty[Dirty::VertexInput], "extended dynamic state needs no tracking");
    for (bool extension : {false, true}) {
        for (u32 attrs : {16U, 31U, 32U, 64U}) {
            for (u32 bindings : {16U, 31U, 32U, 64U}) {
                Device device{extension, attrs, bindings};
                Require(Vulkan::SupportsDirectVertexInput(device) ==
                        (extension && attrs >= 32 && bindings >= 32), "capability matrix");
            }
        }
    }
}

void PipelineStates() {
    for (bool extended : {false, true}) {
        for (bool dynamic : {false, true}) {
            for (bool depth : {false, true}) {
                struct State {
                    bool extended_dynamic_state;
                    bool extended_dynamic_state_2;
                    bool extended_dynamic_state_2_extra;
                    bool extended_dynamic_state_3_blend;
                    bool extended_dynamic_state_3_enables;
                    bool dynamic_vertex_input;
                };
                struct Key { State state; } key{{extended, extended, extended, extended,
                                                 extended, dynamic}};
                Device device;
                device.depth_bounds = depth;
#include "states.inc"
                Require(std::count(dynamic_states.begin(), dynamic_states.end(),
                                   VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) == (dynamic ? 1 : 0),
                        "vertex input declaration independent of extended state");
                Require(std::count(dynamic_states.begin(), dynamic_states.end(),
                                   VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT) ==
                        (extended ? 1 : 0), "dynamic stride declaration");
            }
        }
    }
}

#ifdef VERTEX_INPUT_COMPARE
double Measure(Update update, u32 count, int scenario) {
    auto owned = std::make_unique<RasterizerVulkan>();
    auto& f = *owned;
    Layout(f, count);
    f.buffer_cache_runtime.track_vertex_input_strides = scenario != 4;
    if (scenario == 5) {
        Notify(f, update, 0, 64);
    }
    f.engine.dirty.flags.set();
    (f.*update)(f.engine.regs);
    const u32 iterations = (scenario == 0 || scenario >= 4) ? 3000000 : 500000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < iterations; ++i) {
        if (scenario != 0) {
            if (scenario >= 4) {
                if (f.buffer_cache_runtime.track_vertex_input_strides) {
                    Notify(f, update, 0, 64);
                }
            } else if (scenario == 3) {
                Notify(f, update, 0, (i & 1) ? 64 : 80);
                f.engine.regs.vertex_streams[0].stride.Assign((i & 1) ? 64 : 80);
            } else {
                f.engine.dirty.flags.set(Dirty::VertexInput);
                if (scenario == 1) {
                    f.engine.dirty.flags.set(Dirty::VertexAttribute0 + count - 1);
                    f.engine.regs.vertex_attrib_format[count - 1].offset.Assign(i & 31);
                } else {
                    f.engine.dirty.flags.set(Dirty::VertexBinding0);
                    f.engine.regs.vertex_streams[0].frequency = i & 7;
                }
            }
        }
        (f.*update)(f.engine.regs);
    }
    const auto end = std::chrono::steady_clock::now();
    // Observe the recorded result outside the timed loop.
    Require(f.scheduler.capture.commands != 0, "benchmark observable result");
    return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}
void Benchmark() {
    const std::array<Update, 3> variants{&RasterizerVulkan::Original,
                                      &RasterizerVulkan::Supplied,
                                      &RasterizerVulkan::UpdateVertexInput};
    const char* names[]{"original_invalid", "supplied", "improved"};
    const char* scenarios[]{"clean", "attribute", "divisor", "stride",
                            "rebase_extended", "rebase_tracked"};
    std::mt19937 random(1729);
    std::puts("variant,attributes,scenario,median_ns,min_ns,max_ns");
    for (u32 count : {1U, 8U, 32U}) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            std::array<std::vector<double>, 3> samples;
            for (int repeat = 0; repeat < 11; ++repeat) {
                std::array<int, 3> order{0, 1, 2};
                std::shuffle(order.begin(), order.end(), random);
                for (int variant : order) {
                    samples[variant].push_back(Measure(variants[variant], count, scenario));
                }
            }
            for (int v = 0; v < 3; ++v) {
                auto& s = samples[v];
                std::sort(s.begin(), s.end());
                std::printf("%s,%u,%s,%.3f,%.3f,%.3f\n", names[v], count,
                            scenarios[scenario], s[s.size()/2], s.front(), s.back());
            }
        }
    }
}
#endif

int main(int argc, char** argv) {
#ifdef VERTEX_INPUT_COMPARE
    if (argc > 1 && std::string_view(argv[1]) == "--original") {
        Check(&RasterizerVulkan::Original);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
        Benchmark();
        return 0;
    }
    Check(&RasterizerVulkan::Supplied);
#endif
    Check(&RasterizerVulkan::UpdateVertexInput);
    StridesAndPolicy();
    PipelineStates();
    std::puts("PASS: vertex input regressions and capability matrix");
}
