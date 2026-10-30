// SPDX-License-Identifier: GPL-2.0-or-later
#include "fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/logging/log.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

VideoSettings videoSettings{};
#include "yuzu_video_core/engines/sw_blitter/blitter.h"
#include "yuzu_video_core/textures/decoders.h"

using Tegra::Engines::DmaImageCopySize;
using Tegra::Engines::Fast2DCopySize;
using Tegra::Engines::Fermi2D;
using Clock = std::chrono::steady_clock;
unsigned failures{}, cases{};
void Check(bool ok, const char* message) {
    ++cases;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
Fermi2D::Config Config(u32 width, u32 height) {
    return {.operation = Fermi2D::Operation::SrcCopy,
            .filter = Fermi2D::Filter::Point,
            .must_accelerate = false,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(width),
            .dst_y1 = static_cast<s32>(height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(width),
            .src_y1 = static_cast<s32>(height)};
}
Fermi2D::Surface Surface(u32 width, u32 height, u32 bpp, bool pitch, u32 bh) {
    const auto format = [bpp] {
        using Tegra::RenderTargetFormat;
        switch (bpp) {
        case 1:
            return RenderTargetFormat::R8_UNORM;
        case 2:
            return RenderTargetFormat::R16_UNORM;
        case 4:
            return RenderTargetFormat::A8B8G8R8_UNORM;
        default:
            return RenderTargetFormat::R16G16B16A16_FLOAT;
        }
    }();
    Fermi2D::Surface result{};
    result.format = format;
    result.linear = pitch ? Fermi2D::MemoryLayout::Pitch : Fermi2D::MemoryLayout::BlockLinear;
    result.block_height.Assign(bh);
    result.width = width;
    result.height = height;
    result.depth = 1;
    result.pitch = width * bpp;
    result.addr_lower = CopyFixture::Source;
    return result;
}
void Launch(Fermi2D& engine, const Fermi2D::Surface& src, const Fermi2D::Surface& dst) {
    engine.regs.src = src;
    engine.regs.dst = dst;
    engine.regs.operation = Fermi2D::Operation::SrcCopy;
    auto& args = engine.regs.pixels_from_memory;
    args.dst_width = static_cast<s32>(dst.width);
    args.dst_height = static_cast<s32>(dst.height);
    args.du_dx = 1LL << 32;
    args.dv_dy = 1LL << 32;
    engine.CallMethod(FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1, 0, true);
}
void LayoutTests(CopyFixture& fixture) {
    Tegra::Engines::Blitter::SoftwareBlitEngine reference{fixture.gpu};
    Fermi2D engine{fixture.gpu};
    engine.BindRasterizer(&fixture.rasterizer);
    for (u32 bpp : {1U, 2U, 4U, 8U}) {
        for (bool pitch : {false, true}) {
            for (u32 bh : {0U, 1U, 3U, 5U}) {
                for (u32 width : {17U, 64U, 128U}) {
                    for (u32 height : {7U, 8U, 64U, 256U}) {
                        auto src = Surface(width, height, bpp, pitch, bh);
                        auto dst = src;
                        dst.addr_lower = CopyFixture::Destination;
                        auto config = Config(width, height);
                        const size_t size = pitch ? size_t{src.pitch} * height
                                                  : Tegra::Texture::CalculateSize(true, bpp, width,
                                                                                  height, 1, bh, 0);
                        auto* input = fixture.physical.bytes.data() + CopyFixture::Source;
                        auto* output = fixture.physical.bytes.data() + CopyFixture::Destination;
                        for (size_t i = 0; i < size; ++i)
                            input[i] = static_cast<u8>((i * 37 + i / 97) & 255);
                        std::fill_n(output, size, 0xA5);
                        reference.Blit(src, dst, config);
                        fixture.gpu.FlushCaching();
                        const std::vector<u8> expected(output, output + size);
                        std::fill_n(output, size, 0xA5);
                        Launch(engine, src, dst);
                        fixture.gpu.FlushCaching();
                        Check(std::equal(expected.begin(), expected.end(), output),
                              "real Fermi2D output and padding match software blitter");
                        const bool eligible =
                            pitch || ((width * bpp) % 64 == 0 && height % (8U << bh) == 0);
                        Check((Fast2DCopySize(src, dst, config) != 0) == eligible,
                              "Fermi layout eligibility");
                        if (eligible)
                            Check(Fast2DCopySize(src, dst, config) == size,
                                  "exact allocation size");
                        auto reject = config;
                        reject.src_x0 = 1;
                        Check(!Fast2DCopySize(src, dst, reject), "partial copy rejected");
                        reject = config;
                        reject.must_accelerate = true;
                        Check(!Fast2DCopySize(src, dst, reject), "scaling rejected");
                        reject = config;
                        reject.filter = Fermi2D::Filter::Bilinear;
                        Check(!Fast2DCopySize(src, dst, reject), "filter rejected");
                    }
                }
            }
        }
    }
    auto src = Surface(64, 64, 4, true, 0), dst = src;
    auto config = Config(64, 64);
    dst.pitch += 16;
    Check(!Fast2DCopySize(src, dst, config), "destination pitch padding rejected");
    src.pitch += 16;
    Check(!Fast2DCopySize(src, dst, config), "identical pitch padding rejected");
    src = Surface(0x40000000, 16, 8, false, 0);
    dst = src;
    Check(!Fast2DCopySize(src, dst, Config(src.width, src.height)), "u32 byte overflow rejected");
    src = Surface(64, 64, 4, false, 15);
    dst = src;
    Check(!Fast2DCopySize(src, dst, config), "unsupported block height rejected");
}
void MappingTests(CopyFixture& f) {
    Fermi2D engine{f.gpu};
    engine.BindRasterizer(&f.rasterizer);
    auto src = Surface(64, 64, 4, false, 0), dst = src;
    dst.addr_lower = CopyFixture::Destination;
    auto* input = f.physical.bytes.data() + CopyFixture::Source;
    auto* output = f.physical.bytes.data() + CopyFixture::Destination;
    constexpr size_t size = 64 * 64 * 4;
    for (size_t i = 0; i < size; ++i)
        input[i] = static_cast<u8>(i * 7);
    // Two GPU VAs sharing the same device address: fallback must retain snapshot semantics.
    f.gpu.Map(0x4000000, CopyFixture::Source, CopyFixture::RegionSize);
    dst.addr_lower = 0x4000000;
    Launch(engine, src, dst);
    f.gpu.FlushCaching();
    Check(std::equal(input, input + size, f.gpu.GetPointer(dst.Address())), "GPU VA alias copy");
    // Distinct device ranges backed by the same physical memory, with partial overlap.
    const auto asid = f.device.RegisterProcess(&f.physical);
    f.device.Map(0x3000000, CopyFixture::Source + 4096, CopyFixture::RegionSize, asid, true);
    f.gpu.Map(0x5000000, 0x3000000, CopyFixture::RegionSize);
    const std::vector<u8> snapshot(input, input + size);
    dst.addr_lower = 0x5000000;
    Launch(engine, src, dst);
    f.gpu.FlushCaching();
    Check(std::equal(snapshot.begin(), snapshot.end(), input + 4096),
          "physical overlap uses software snapshot");
    // A discontinuity in device addresses must preserve per-page mapping semantics.
    f.gpu.Map(0x6000000, CopyFixture::Source, 8192, Tegra::PTEKind::INVALID, false);
    f.gpu.Map(0x6002000, CopyFixture::Source + 16384, 8192, Tegra::PTEKind::INVALID, false);
    src.addr_lower = 0x6000000;
    dst.addr_lower = CopyFixture::Destination;
    std::vector<u8> expected(size);
    f.gpu.ReadBlock(src.Address(), expected.data(), size);
    Launch(engine, src, dst);
    f.gpu.FlushCaching();
    Check(std::equal(expected.begin(), expected.end(), output), "discontinuous GPU mapping");
    // GPU dirty reports force the ordinary software/cache route.
    f.rasterizer.dirty = true;
    src.addr_lower = CopyFixture::Source;
    Launch(engine, src, dst);
    f.gpu.FlushCaching();
    Check(std::equal(input, input + size, output), "GPU dirty fallback");
    f.rasterizer.dirty = false;
    Launch(engine, src, src);
    Check(std::equal(input, input + size, f.gpu.GetPointer(src.Address())), "identity copy");
}
void LaunchDMA(Tegra::Engines::MaxwellDMA& engine, const Fermi2D::Surface& src,
               const Fermi2D::Surface& dst, u32 bpp, bool remap = false) {
    using DMA = Tegra::Engines::MaxwellDMA;
    // Program the public method interface using the register offsets asserted
    // in maxwell_dma.h, exactly as a guest command stream does.
    const auto params = [&](const Fermi2D::Surface& surface, u32 offset) {
        Tegra::DMA::Parameters p{};
        p.width = surface.width * bpp;
        p.height = surface.height;
        p.depth = 1;
        p.block_size.height.Assign(surface.block_height);
        const auto words = std::bit_cast<std::array<u32, 6>>(p);
        for (u32 i = 0; i < words.size(); ++i)
            engine.CallMethod(offset / 4 + i, words[i], true);
    };
    params(src, 0x728);
    params(dst, 0x70C);
    engine.CallMethod(0x400 / 4, src.addr_upper, true);
    engine.CallMethod(0x404 / 4, src.addr_lower, true);
    engine.CallMethod(0x408 / 4, dst.addr_upper, true);
    engine.CallMethod(0x40C / 4, dst.addr_lower, true);
    engine.CallMethod(0x418 / 4, src.width * bpp, true);
    engine.CallMethod(0x41C / 4, src.height, true);
    DMA::LaunchDMA launch{};
    launch.multi_line_enable.Assign(1);
    launch.remap_enable.Assign(remap ? 1 : 0);
    launch.data_transfer_type.Assign(DMA::LaunchDMA::DataTransferType::NON_PIPELINED);
    engine.CallMethod(0x300 / 4, std::bit_cast<u32>(launch), true);
}
void DmaEngineTests(CopyFixture& f) {
    Tegra::Engines::MaxwellDMA engine{f.gpu};
    engine.BindRasterizer(&f.rasterizer);
    Tegra::Engines::Blitter::SoftwareBlitEngine reference{f.gpu};
    for (u32 bpp : {1U, 2U, 4U, 8U}) {
        for (u32 width : {17U, 64U, 128U}) {
            for (u32 height : {7U, 64U, 256U}) {
                for (u32 bh : {0U, 2U, 5U}) {
                    auto src = Surface(width, height, bpp, false, bh), dst = src;
                    dst.addr_lower = CopyFixture::Destination;
                    auto config = Config(width, height);
                    const size_t size =
                        Tegra::Texture::CalculateSize(true, bpp, width, height, 1, bh, 0);
                    auto* input = f.physical.bytes.data() + CopyFixture::Source;
                    auto* output = f.physical.bytes.data() + CopyFixture::Destination;
                    for (size_t i = 0; i < size; ++i)
                        input[i] = static_cast<u8>(i * 37 + i / 97);
                    std::fill_n(output, size, 0xA5);
                    reference.Blit(src, dst, config);
                    f.gpu.FlushCaching();
                    const std::vector<u8> expected(output, output + size);
                    std::fill_n(output, size, 0xA5);
                    LaunchDMA(engine, src, dst, bpp);
                    f.gpu.FlushCaching();
                    Check(std::equal(expected.begin(), expected.end(), output),
                          "real DMA block copy matches software and preserves padding");
                    std::fill_n(output, size, 0xA5);
                    f.rasterizer.dirty = true;
                    LaunchDMA(engine, src, dst, bpp);
                    f.gpu.FlushCaching();
                    f.rasterizer.dirty = false;
                    Check(std::equal(expected.begin(), expected.end(), output),
                          "DMA GPU-dirty fallback");
                    // Identity remap still takes the established software path.
                    std::fill_n(output, size, 0xA5);
                    const auto attempts = f.rasterizer.dma.image_attempts;
                    LaunchDMA(engine, src, dst, bpp, true);
                    f.gpu.FlushCaching();
                    Check(std::equal(expected.begin(), expected.end(), output),
                          "remap identity matches reference");
                    Check(f.rasterizer.dma.image_attempts == attempts,
                          "remapped DMA skips both fast paths");
                }
            }
        }
    }
}
void DmaTests() {
    for (u32 bpp : {1U, 2U, 4U, 8U}) {
        for (u32 width : {17U, 64U, 1920U}) {
            for (u32 height : {7U, 64U, 720U}) {
                for (u32 bh = 0; bh <= 5; ++bh) {
                    Tegra::DMA::ImageOperand src{};
                    src.bytes_per_pixel = 1;
                    src.params.width = width * bpp;
                    src.params.height = height;
                    src.params.depth = 1;
                    src.params.block_size.height.Assign(bh);
                    auto dst = src;
                    Tegra::DMA::ImageCopy copy{width * bpp, height};
                    Check(DmaImageCopySize(copy, src, dst) ==
                              Tegra::Texture::CalculateSize(true, bpp, width, height, 1, bh, 0),
                          "DMA byte-to-texel layout including GOB padding");
                    auto bad = dst;
                    bad.params.origin.x.Assign(1);
                    Check(!DmaImageCopySize(copy, src, bad), "DMA destination origin rejected");
                    bad = dst;
                    bad.params.depth = 2;
                    Check(!DmaImageCopySize(copy, src, bad), "DMA volume rejected");
                    bad = dst;
                    bad.params.block_size.height.Assign((bh + 1) % 6);
                    Check(!DmaImageCopySize(copy, src, bad), "DMA different block height rejected");
                    bad = dst;
                    bad.params.layer = 1;
                    Check(!DmaImageCopySize(copy, src, bad), "DMA layer rejected");
                    --copy.length_x;
                    Check(!DmaImageCopySize(copy, src, dst), "DMA partial copy rejected");
                }
            }
        }
    }
    Tegra::DMA::ImageOperand src{};
    src.bytes_per_pixel = 1;
    src.params.width = 0x7fffffff;
    src.params.height = 0x7fffffff;
    src.params.depth = 1;
    Check(!DmaImageCopySize({src.params.width, src.params.height}, src, src),
          "DMA aligned-size overflow rejected");
    src.params.width = 64;
    src.params.height = 64;
    src.params.block_size.height.Assign(15);
    Check(!DmaImageCopySize({64, 64}, src, src), "DMA invalid block height rejected before shifts");
}

struct Timing {
    double median, p95;
};
template <typename F> Timing Time(F&& run, unsigned iterations = 101) {
    std::vector<double> samples;
    for (unsigned i = 0; i < 12; ++i)
        run();
    for (unsigned i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        run();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[samples.size() * 95 / 100]};
}
void Benchmark(CopyFixture& f) {
    Tegra::Engines::Blitter::SoftwareBlitEngine reference{f.gpu};
    Fermi2D engine{f.gpu};
    engine.BindRasterizer(&f.rasterizer);
    std::puts("mode,width,height,bpp,repeat,software_median_us,fast_median_us,software_p95_us,fast_"
              "p95_us,speedup");
    for (auto dims : {std::array<u32, 2>{256, 256}, {1280, 768}, {1920, 1088}}) {
        for (bool pitch : {false, true}) {
            for (u32 bpp : {1U, 2U, 4U, 8U}) {
                auto src = Surface(dims[0], dims[1], bpp, pitch, 3), dst = src;
                dst.addr_lower = CopyFixture::Destination;
                auto config = Config(src.width, src.height);
                Check(Fast2DCopySize(src, dst, config) != 0, "benchmark must enter fast path");
                const auto size = Fast2DCopySize(src, dst, config);
                auto* input = f.physical.bytes.data() + CopyFixture::Source;
                for (size_t i = 0; i < size; ++i)
                    input[i] = static_cast<u8>(i * 37 + i / 97);
                for (unsigned repeat = 0; repeat < 3; ++repeat) {
                    const auto baseline = [&] {
                        reference.Blit(src, dst, config);
                        f.gpu.FlushCaching();
                    };
                    const auto fast = [&] {
                        Launch(engine, src, dst);
                        f.gpu.FlushCaching();
                    };
                    Timing before{}, after{};
                    if (repeat % 2) {
                        after = Time(fast);
                        before = Time(baseline);
                    } else {
                        before = Time(baseline);
                        after = Time(fast);
                    }
                    std::printf("%s,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.2f\n",
                                pitch ? "pitch" : "block", src.width, src.height, bpp, repeat,
                                before.median, after.median, before.p95, after.p95,
                                before.median / after.median);
                }
            }
        }
    }
    Tegra::Engines::MaxwellDMA dma{f.gpu};
    dma.BindRasterizer(&f.rasterizer);
    for (auto dims : {std::array<u32, 2>{256, 256}, {1280, 768}, {1920, 1088}}) {
        for (u32 bpp : {1U, 2U, 4U, 8U}) {
            auto src = Surface(dims[0], dims[1], bpp, false, 3), dst = src;
            dst.addr_lower = CopyFixture::Destination;
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                // A dirty report disables the direct memory copy, exercising the
                // real previous deswizzle/reswizzle route with no actual GPU work.
                f.rasterizer.dirty = true;
                const auto before = Time([&] {
                    LaunchDMA(dma, src, dst, bpp);
                    f.gpu.FlushCaching();
                });
                f.rasterizer.dirty = false;
                const auto after = Time([&] {
                    LaunchDMA(dma, src, dst, bpp);
                    f.gpu.FlushCaching();
                });
                std::printf("dma_block,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.2f\n", src.width,
                            src.height, bpp, repeat, before.median, after.median, before.p95,
                            after.p95, before.median / after.median);
            }
        }
    }
    // Include rejected copies: compare the full engine (failed guard + fallback)
    // with the original software route. No cache lookup is faked as successful.
    auto src = Surface(257, 256, 4, true, 0), dst = src;
    src.pitch += 16;
    dst.pitch += 16;
    dst.addr_lower = CopyFixture::Destination;
    auto config = Config(src.width, src.height);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const auto before = Time([&] {
            reference.Blit(src, dst, config);
            f.gpu.FlushCaching();
        });
        const auto after = Time([&] {
            Launch(engine, src, dst);
            f.gpu.FlushCaching();
        });
        std::printf("fallback,%u,%u,4,%u,%.3f,%.3f,%.3f,%.3f,%.2f\n", src.width, src.height, repeat,
                    before.median, after.median, before.p95, after.p95,
                    before.median / after.median);
    }
}
int main(int argc, char** argv) {
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    CopyFixture fixture;
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark")
        Benchmark(fixture);
    else {
        LayoutTests(fixture);
        MappingTests(fixture);
        DmaTests();
        DmaEngineTests(fixture);
    }
    std::fprintf(stderr, "%u checks, %u failures\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
