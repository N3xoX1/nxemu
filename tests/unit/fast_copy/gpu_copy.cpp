// SPDX-License-Identifier: GPL-2.0-or-later
// Manual headless integration test. Uses the real Vulkan runtime and texture cache.
#include "fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/sw_blitter/blitter.h"
#include "yuzu_video_core/renderer_vulkan/blit_image.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

IModuleSettings* g_settings{};
IModuleNotification* g_notify{};

using Fermi = Tegra::Engines::Fermi2D;
void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
Fermi::Surface Surface(GPUVAddr addr, u32 width, u32 height, Tegra::RenderTargetFormat format) {
    Fermi::Surface surface{};
    surface.format = format;
    surface.width = width;
    surface.height = height;
    surface.depth = 1;
    surface.addr_lower = static_cast<u32>(addr);
    surface.addr_upper = static_cast<u32>(addr >> 32);
    return surface;
}
Fermi::Config Config(u32 width, u32 height) {
    return {.operation = Fermi::Operation::SrcCopy,
            .filter = Fermi::Filter::Point,
            .must_accelerate = true,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(width),
            .dst_y1 = static_cast<s32>(height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(width),
            .src_y1 = static_cast<s32>(height)};
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: %s /path/to/Vulkan/library\n", argv[0]);
        return 2;
    }
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    Common::DynamicLibrary library{argv[1]};
    Require(library.IsOpen(), "Vulkan library opens");
    Vulkan::vk::InstanceDispatch dispatch;
    auto instance = Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1);
    const auto devices = instance.EnumeratePhysicalDevices();
    Require(!devices.empty(), "physical device available");
    Vulkan::Device device{*instance, Vulkan::vk::PhysicalDevice{devices[0], dispatch}, nullptr,
                          dispatch};
    CopyFixture memory;
    Tegra::Control::ChannelState channel{0};
    channel.memory_manager = std::shared_ptr<Tegra::MemoryManager>(&memory.gpu, [](auto*) {});
    channel.maxwell_3d = std::make_unique<Tegra::Engines::Maxwell3D>(memory.gpu);
    channel.kepler_compute = std::make_unique<Tegra::Engines::KeplerCompute>(memory.gpu);
    Vulkan::StateTracker state;
    state.SetupTables(channel);
    state.ChangeChannel(channel);
    Vulkan::Scheduler scheduler{device, state};
    Vulkan::MemoryAllocator allocator{device};
    Vulkan::StagingBufferPool staging{device, allocator, scheduler};
    Vulkan::DescriptorPool descriptors{device, scheduler};
    Vulkan::ComputePassDescriptorQueue descriptor_queue{device, scheduler};
    Vulkan::BlitImageHelper blitter{device, scheduler, state, descriptors};
    Vulkan::RenderPassCache render_passes{device};
    Vulkan::TextureCacheRuntime runtime{device,  scheduler,     allocator,   staging,
                                        blitter, render_passes, descriptors, descriptor_queue};
    Vulkan::TextureCache cache{runtime, memory.device};
    cache.CreateChannel(channel);
    cache.BindToChannel(0);
    using Tegra::RenderTargetFormat;
    constexpr std::array formats{RenderTargetFormat::R8_UNORM, RenderTargetFormat::R16_UNORM,
                                 RenderTargetFormat::A8B8G8R8_UNORM,
                                 RenderTargetFormat::R16G16B16A16_FLOAT};
    unsigned checks{};
    for (u32 bpp : {1U, 2U, 4U, 8U}) {
        for (u32 width : {64U, 257U}) {
            constexpr u32 height = 67;
            const auto format = formats[std::countr_zero(bpp)];
            auto source = Surface(CopyFixture::Source, width, height, format);
            auto destination = Surface(CopyFixture::Destination, width, height, format);
            auto consumer = Surface(CopyFixture::Destination + 0x200000, width, height, format);
            auto rendered = Surface(CopyFixture::Source + 0x200000, width, height, format);
            auto config = Config(width, height);
            const size_t size = Tegra::Texture::CalculateSize(true, bpp, width, height, 1, 0, 0);
            std::vector<u8> texels(size_t{width} * height * bpp);
            std::vector<u8> expected(size, 0xA5);
            for (size_t i = 0; i < texels.size(); ++i)
                texels[i] = static_cast<u8>(i * 37 + i / 97);
            if (bpp == 8) {
                // The setup blit uses floating-point attachments. Keep half-float
                // values finite and exactly representable so setup is bit exact.
                constexpr std::array<u16, 4> values{0, 0x3800, 0x3C00, 0xBC00};
                for (size_t i = 0; i < texels.size() / 2; ++i) {
                    const u16 value = values[i % values.size()];
                    std::memcpy(texels.data() + i * 2, &value, sizeof(value));
                }
            }
            Tegra::Texture::SwizzleTexture(expected, texels, bpp, width, height, 1, 0, 0);
            memory.gpu.WriteBlockUnsafe(source.Address(), expected.data(), size);
            std::vector<u8> blank(size, 0xA5);
            memory.gpu.WriteBlockUnsafe(destination.Address(), blank.data(), size);
            memory.gpu.WriteBlockUnsafe(consumer.Address(), blank.data(), size);
            // Seed the source through a real GPU blit. Guest RAM at rendered stays stale.
            Require(cache.BlitImage(rendered, source, config), "GPU source blit");
            Require(cache.BlitImage(destination, consumer, config), "cache destination");
            Tegra::DMA::ImageOperand src{};
            src.bytes_per_pixel = 1;
            src.params.width = width * bpp;
            src.params.height = height;
            src.params.depth = 1;
            src.address = rendered.Address();
            auto dst = src;
            dst.address = destination.Address();
            const Tegra::DMA::ImageCopy copy{width * bpp, height};
            Require(Tegra::Engines::DmaImageCopySize(copy, src, dst) != 0, "DMA layout eligible");
            const auto tick = scheduler.CurrentTick();
            Require(cache.DmaImageToImage(copy, src, dst), "real DMA image-to-image fast path");
            Require(scheduler.CurrentTick() == tick, "DMA must not submit/wait for a CPU readback");
            // Consume the DMA output immediately on the GPU before any CPU readback.
            Require(cache.GetFlushArea(destination.Address(), size).has_value(),
                    "observe GPU-written destination");
            Require(cache.BlitImage(consumer, destination, config), "immediate GPU consumer");
            cache.DownloadMemory(consumer.Address(), size);
            std::vector<u8> actual(size);
            memory.gpu.ReadBlockUnsafe(consumer.Address(), actual.data(), size);
            Require(actual == expected, "GPU consumer and CPU readback, padding included");
            ++checks;
            // Source CPU modification must upload fresh texels before the host image copy.
            std::reverse(texels.begin(), texels.end());
            Tegra::Texture::SwizzleTexture(expected, texels, bpp, width, height, 1, 0, 0);
            memory.gpu.WriteBlockUnsafe(rendered.Address(), expected.data(), size);
            cache.WriteMemory(rendered.Address(), size);
            // Destination CPU-dirty state must be discarded by a complete overwrite.
            memory.gpu.WriteBlockUnsafe(destination.Address(), blank.data(), size);
            cache.WriteMemory(destination.Address(), size);
            Require(cache.DmaImageToImage(copy, src, dst), "CPU-dirty source/destination DMA");
            Require(cache.HasUncommittedFlushes(),
                    "DMA queues observed destination for preemptive download");
            cache.CommitAsyncFlushes();
            scheduler.Finish();
            cache.PopAsyncFlushes();
            cache.DownloadMemory(destination.Address(), size);
            memory.gpu.ReadBlockUnsafe(destination.Address(), actual.data(), size);
            Require(actual == expected, "CPU-dirty transitions preserve all bytes");
            ++checks;
            Require(!cache.DmaImageToImage(copy, src, src), "same image rejected");
            ++checks;
            auto absent = dst;
            absent.address += 0x400000;
            Require(!cache.DmaImageToImage(copy, src, absent), "uncached destination rejected");
            ++checks;
            auto alias = dst;
            alias.address = 0x4000000;
            memory.gpu.Map(alias.address, rendered.Address(), 0x10000);
            Require(!cache.DmaImageToImage(copy, src, alias),
                    "aliased GPU virtual address rejected");
            ++checks;
            cache.UnmapMemory(source.Address(), 0x400000);
            cache.UnmapMemory(destination.Address(), 0x400000);
            scheduler.Finish();
        }
    }
    scheduler.Finish();
    std::fprintf(stderr, "%u GPU cache/readback checks passed\n", checks);
    if (argc == 3) {
        using Clock = std::chrono::steady_clock;
        Tegra::Engines::Blitter::SoftwareBlitEngine software{memory.gpu};
        std::puts("width,height,repeat,software_median_us,gpu_median_us,software_p95_us,gpu_p95_us,"
                  "cpu_speedup,software_completed_batch_us_per_copy,gpu_completed_batch_us_per_"
                  "copy,completed_batch_speedup");
        for (auto dims : {std::array<u32, 2>{256, 256}, {1280, 720}, {1920, 1080}}) {
            const auto width = dims[0], height = dims[1];
            auto source =
                Surface(CopyFixture::Source, width, height, RenderTargetFormat::A8B8G8R8_UNORM);
            auto rendered = source;
            rendered.addr_lower += 8 * 1024 * 1024;
            auto destination = source;
            destination.addr_lower = CopyFixture::Destination;
            auto consumer = destination;
            consumer.addr_lower += 8 * 1024 * 1024;
            auto config = Config(width, height);
            const size_t size = Tegra::Texture::CalculateSize(true, 4, width, height, 1, 0, 0);
            std::vector<u8> input(size);
            for (size_t i = 0; i < size; ++i)
                input[i] = static_cast<u8>(i * 37 + i / 97);
            memory.gpu.WriteBlockUnsafe(source.Address(), input.data(), size);
            Require(cache.BlitImage(rendered, source, config), "benchmark seed source");
            Require(cache.BlitImage(destination, source, config), "benchmark seed destination");
            Tegra::DMA::ImageOperand src{};
            src.bytes_per_pixel = 1;
            src.params.width = width * 4;
            src.params.height = height;
            src.params.depth = 1;
            src.address = rendered.Address();
            auto dst = src;
            dst.address = destination.Address();
            const Tegra::DMA::ImageCopy copy{width * 4, height};
            const auto measure = [&](bool accelerated) {
                std::vector<double> samples;
                scheduler.Finish();
                Clock::time_point batch_start;
                for (unsigned iteration = 0; iteration < 65; ++iteration) {
                    if (iteration == 14) {
                        scheduler.Finish();
                        batch_start = Clock::now();
                    }
                    // Both routes start with a fresh GPU write, outside the timed CPU section.
                    Require(cache.BlitImage(rendered, source, config), "benchmark GPU producer");
                    const auto start = Clock::now();
                    if (accelerated) {
                        Require(cache.DmaImageToImage(copy, src, dst), "benchmark DMA fast path");
                    } else {
                        cache.DownloadMemory(rendered.Address(), size);
                        software.Blit(rendered, destination, config);
                        memory.gpu.FlushCaching();
                        cache.WriteMemory(destination.Address(), size);
                    }
                    Require(cache.BlitImage(consumer, destination, config),
                            "benchmark GPU consumer");
                    const double us =
                        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
                    if (iteration >= 14)
                        samples.push_back(us);
                }
                scheduler.Finish();
                std::sort(samples.begin(), samples.end());
                const double completed_us =
                    std::chrono::duration<double, std::micro>(Clock::now() - batch_start).count() /
                    samples.size();
                return std::array{samples[samples.size() / 2], samples[samples.size() * 95 / 100],
                                  completed_us};
            };
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                std::array<double, 3> before{}, after{};
                if (repeat % 2) {
                    after = measure(true);
                    before = measure(false);
                } else {
                    before = measure(false);
                    after = measure(true);
                }
                std::printf("%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.2f,%.3f,%.3f,%.2f\n", width, height,
                            repeat, before[0], after[0], before[1], after[1], before[0] / after[0],
                            before[2], after[2], before[2] / after[2]);
            }
            cache.UnmapMemory(CopyFixture::Source, CopyFixture::RegionSize);
            cache.UnmapMemory(CopyFixture::Destination, CopyFixture::RegionSize);
            scheduler.Finish();
        }
    }
}
