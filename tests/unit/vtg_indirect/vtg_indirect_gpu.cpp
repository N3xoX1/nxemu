// SPDX-License-Identifier: GPL-2.0-or-later
// Manual headless integration test. Runs the indirect draw preparation of the stages running
// as compute on a real Vulkan device and checks it against the CPU primitive count.
#include "fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pass.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

IModuleSettings* g_settings{};
IModuleNotification* g_notify{};

namespace {

using Shader::VTG_LOCAL_SIZE;
using Shader::VtgTopology;
namespace Info = Shader::VtgDrawInfo;

unsigned failures{}, cases{};
void Check(bool ok, const char* message, VtgTopology topology, u32 count, u32 instances,
           u32 capacity, u32 mask) {
    ++cases;
    if (!ok) {
        ++failures;
        std::fprintf(stderr,
                     "FAIL: %s (topology=%u count=%u instances=%u capacity=%u mask=%08x)\n",
                     message, static_cast<u32>(topology), count, instances, capacity, mask);
    }
}

constexpr u32 SENTINEL = 0xDEADBEEF;
constexpr u32 ARGS_WORD = 3; // the arguments do not start the bound range
constexpr u32 COMMAND_WORDS = 11;

struct Mapped {
    Vulkan::vk::Buffer buffer;
    u32 size;

    u32* Words() {
        return reinterpret_cast<u32*>(buffer.Mapped().data());
    }
    VkDescriptorBufferInfo Descriptor() const {
        return {*buffer, 0, size};
    }
};

Mapped CreateBuffer(Vulkan::MemoryAllocator& allocator, u32 size) {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    return {allocator.CreateBuffer(ci, Vulkan::MemoryUsage::Download), size};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /path/to/Vulkan/library\n", argv[0]);
        return 2;
    }
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    Common::DynamicLibrary library{argv[1]};
    if (!library.IsOpen()) {
        std::fprintf(stderr, "FAIL: Vulkan library opens\n");
        return 1;
    }
    Vulkan::vk::InstanceDispatch dispatch;
    auto instance = Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1);
    const auto devices = instance.EnumeratePhysicalDevices();
    if (devices.empty()) {
        std::fprintf(stderr, "FAIL: physical device available\n");
        return 1;
    }
    Vulkan::Device device{*instance, Vulkan::vk::PhysicalDevice{devices[0], dispatch}, nullptr,
                          dispatch};
    EngineFixture memory;
    Tegra::Control::ChannelState channel{0};
    channel.memory_manager = std::shared_ptr<Tegra::MemoryManager>(&memory.gpu, [](auto*) {});
    channel.maxwell_3d = std::make_unique<Tegra::Engines::Maxwell3D>(memory.gpu);
    channel.kepler_compute = std::make_unique<Tegra::Engines::KeplerCompute>(memory.gpu);
    Vulkan::StateTracker state;
    state.SetupTables(channel);
    state.ChangeChannel(channel);
    Vulkan::Scheduler scheduler{device, state};
    Vulkan::MemoryAllocator allocator{device};
    Vulkan::DescriptorPool descriptors{device, scheduler};
    Vulkan::VtgIndirectPass pass{device, descriptors};

    auto args = CreateBuffer(allocator, (ARGS_WORD + 4) * sizeof(u32));
    auto info = CreateBuffer(allocator, Info::NUM_WORDS * sizeof(u32));
    auto commands = CreateBuffer(allocator, COMMAND_WORDS * sizeof(u32));
    auto feedback = CreateBuffer(allocator, sizeof(u32));

    constexpr std::array topologies{
        VtgTopology::Points,        VtgTopology::Lines,
        VtgTopology::LineLoop,      VtgTopology::LineStrip,
        VtgTopology::Triangles,     VtgTopology::TriangleStrip,
        VtgTopology::TriangleFan,   VtgTopology::Quads,
        VtgTopology::QuadStrip,     VtgTopology::LinesAdjacency,
        VtgTopology::LineStripAdjacency, VtgTopology::TrianglesAdjacency,
        VtgTopology::TriangleStripAdjacency,
    };
    constexpr std::array counts{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 31U, 32U, 33U, 1000U, 1024U, 1025U};
    constexpr std::array instance_counts{0U, 1U, 2U, 3U, 64U};
    constexpr std::array capacities{32U, 1024U};
    // The guest macro masks the instance count it reads
    constexpr std::array masks{0xFFFFFFFFU, 0U, 0x2U};
    for (const VtgTopology topology : topologies) {
        for (const u32 count : counts) {
            for (const u32 instances : instance_counts) {
                for (const u32 capacity : capacities) {
                    for (const u32 mask : masks) {
                        const u32 indices_per_primitive = topology == VtgTopology::Points ? 1 : 3;
                        const u32 first = 17;
                        const u32 base_instance = 5;
                        u32* const a = args.Words();
                        std::fill_n(a, ARGS_WORD, SENTINEL);
                        a[ARGS_WORD] = count;
                        a[ARGS_WORD + 1] = instances;
                        a[ARGS_WORD + 2] = first;
                        a[ARGS_WORD + 3] = base_instance;
                        args.buffer.Flush();
                        std::fill_n(info.Words(), Info::NUM_WORDS, SENTINEL);
                        info.buffer.Flush();
                        std::fill_n(commands.Words(), COMMAND_WORDS, SENTINEL);
                        commands.buffer.Flush();
                        // A previous, larger request must survive
                        *feedback.Words() = 4096;
                        feedback.buffer.Flush();

                        const std::array buffers{args.Descriptor(), info.Descriptor(),
                                                 commands.Descriptor(), feedback.Descriptor()};
                        const std::array parameters{ARGS_WORD, static_cast<u32>(topology), capacity,
                                                    indices_per_primitive, mask};
                        scheduler.Record(
                            [&pass, buffers, parameters](Vulkan::vk::CommandBuffer cmdbuf) {
                                pass.Record(cmdbuf, buffers, parameters);
                            });
                        scheduler.Finish();
                        info.buffer.Invalidate();
                        commands.buffer.Invalidate();
                        feedback.buffer.Invalidate();

                        const u32 per_instance = Shader::VtgPrimitiveCount(topology, count);
                        const u32 masked = instances & mask;
                        const u32 drawn = count == 0 ? 0 : std::min(masked, capacity / count);
                        const u32 vertices = count * drawn;
                        const u32 primitives = per_instance * drawn;
                        const u32* const i = info.Words();
                        const u32* const c = commands.Words();
                        const auto check = [&](bool ok, const char* message) {
                            Check(ok, message, topology, count, instances, capacity, mask);
                        };
                        check(i[Info::VERTEX_COUNT] == count, "vertex count");
                        check(i[Info::INSTANCE_COUNT] == std::max(drawn, 1U), "instance count");
                        check(i[Info::BASE_VERTEX] == first, "base vertex");
                        check(i[Info::BASE_INSTANCE] == base_instance, "base instance");
                        check(i[Info::PRIMITIVE_COUNT] == per_instance, "primitives per instance");
                        bool untouched = true;
                        for (u32 word = 0; word < Info::NUM_WORDS; ++word) {
                            if (word != Info::VERTEX_COUNT && word != Info::INSTANCE_COUNT &&
                                word != Info::BASE_VERTEX && word != Info::BASE_INSTANCE &&
                                word != Info::PRIMITIVE_COUNT) {
                                untouched &= i[word] == SENTINEL;
                            }
                        }
                        check(untouched, "other draw information is kept");
                        check(vertices <= capacity && primitives <= capacity,
                              "the draw fits the capacity");
                        const u32 vertex_groups =
                            primitives == 0 ? 0 : (vertices + VTG_LOCAL_SIZE - 1) / VTG_LOCAL_SIZE;
                        check(c[0] == vertex_groups && c[1] == 1 && c[2] == 1, "vertex dispatch");
                        check(c[3] == (primitives + VTG_LOCAL_SIZE - 1) / VTG_LOCAL_SIZE &&
                                  c[4] == 1 && c[5] == 1,
                              "geometry dispatch");
                        check(c[6] == primitives * indices_per_primitive && c[7] == 1 &&
                                  c[8] == 0 && c[9] == 0 && c[10] == 0,
                              "indexed draw");
                        check(*feedback.Words() == std::max(4096U, count * masked),
                              "largest request is reported");
                    }
                    }
            }
        }
    }
    // A request whose vertex total overflows reports the largest value
    {
        u32* const a = args.Words();
        a[ARGS_WORD] = 0x10000;
        a[ARGS_WORD + 1] = 0x10000;
        a[ARGS_WORD + 2] = 0;
        a[ARGS_WORD + 3] = 0;
        args.buffer.Flush();
        *feedback.Words() = 0;
        feedback.buffer.Flush();
        const std::array buffers{args.Descriptor(), info.Descriptor(), commands.Descriptor(),
                                 feedback.Descriptor()};
        const std::array parameters{ARGS_WORD, static_cast<u32>(VtgTopology::Points), 1024U, 1U,
                                    0xFFFFFFFFU};
        scheduler.Record([&pass, buffers, parameters](Vulkan::vk::CommandBuffer cmdbuf) {
            pass.Record(cmdbuf, buffers, parameters);
        });
        scheduler.Finish();
        feedback.buffer.Invalidate();
        commands.buffer.Invalidate();
        Check(*feedback.Words() == 0xFFFFFFFF, "overflowing request saturates",
              VtgTopology::Points, 0x10000, 0x10000, 1024, 0xFFFFFFFF);
        Check(commands.Words()[6] == 0, "a single instance over capacity draws nothing",
              VtgTopology::Points, 0x10000, 0x10000, 1024, 0xFFFFFFFF);
    }
    std::fprintf(stderr, "%u checks, %u failures\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
