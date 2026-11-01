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
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

VideoSettings videoSettings{};
#include "yuzu_video_core/engines/sw_blitter/blitter.h"
#include "yuzu_video_core/engines/dma_remap.h"
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
                    Check(f.rasterizer.dma.image_attempts == attempts + 1,
                          "identity remap tries the image-to-image fast path");
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

// Reference remap semantics, independent of MaxwellDMA's implementation: every
// destination element of num_dst components is built from the source element
// of num_src components, a constant, or the previous destination value.
using Remap = Tegra::Engines::MaxwellDMA::RemapConst;
Remap MakeRemap(u32 size, u32 nsrc, u32 ndst, std::array<u32, 4> swizzle) {
    Remap r{};
    r.remap_consta_value = 0x11223344;
    r.remap_constb_value = 0x55667788;
    r.component_size_minus_one.Assign(size - 1);
    r.num_src_components_minus_one.Assign(nsrc - 1);
    r.num_dst_components_minus_one.Assign(ndst - 1);
    r.dst_x.Assign(static_cast<Remap::Swizzle>(swizzle[0]));
    r.dst_y.Assign(static_cast<Remap::Swizzle>(swizzle[1]));
    r.dst_z.Assign(static_cast<Remap::Swizzle>(swizzle[2]));
    r.dst_w.Assign(static_cast<Remap::Swizzle>(swizzle[3]));
    return r;
}
// Applies the remap to linear element arrays.
void ReferenceRemap(const Remap& r, std::span<const u8> src, std::span<u8> dst, u32 elements) {
    const u32 size = r.component_size_minus_one + 1;
    const u32 nsrc = r.num_src_components_minus_one + 1;
    const u32 ndst = r.num_dst_components_minus_one + 1;
    const std::array<Remap::Swizzle, 4> swizzle{r.dst_x, r.dst_y, r.dst_z, r.dst_w};
    for (u32 e = 0; e < elements; ++e) {
        for (u32 c = 0; c < ndst; ++c) {
            u8* out = dst.data() + (size_t{e} * ndst + c) * size;
            const u32 sw = static_cast<u32>(swizzle[c]);
            if (sw <= 3) {
                // A component outside the source element reads zero.
                if (sw < nsrc)
                    std::memcpy(out, src.data() + (size_t{e} * nsrc + sw) * size, size);
                else
                    std::memset(out, 0, size);
            } else if (sw == 4 || sw == 5) {
                const u32 value = sw == 4 ? r.remap_consta_value : r.remap_constb_value;
                std::memcpy(out, &value, size);
            }
        }
    }
}
void RemapIdentityTests() {
    using Tegra::Engines::IdentityRemapBytesPerPixel;
    constexpr u32 elements = 13; // 13 * bpp exercises SIMD chunks and a scalar tail
    std::vector<u8> src(elements * 16), dst(elements * 16), before(elements * 16);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<u8>(i * 73 + 5);
        before[i] = static_cast<u8>(i * 151 + 9);
    }
    unsigned accepted{}, rejected_equal{};
    for (u32 size = 1; size <= 4; ++size) {
        for (u32 nsrc = 1; nsrc <= 4; ++nsrc) {
            for (u32 ndst = 1; ndst <= 4; ++ndst) {
                for (u32 code = 0; code < 7 * 7 * 7 * 7; ++code) {
                    std::array<u32, 4> sw{code % 7, code / 7 % 7, code / 49 % 7, code / 343};
                    // Inactive components do not affect the result: only enumerate them once.
                    bool redundant = false;
                    for (u32 c = ndst; c < 4; ++c)
                        redundant |= sw[c] != 0;
                    if (redundant)
                        continue;
                    const Remap r = MakeRemap(size, nsrc, ndst, sw);
                    dst = before;
                    ReferenceRemap(r, src, dst, elements);
                    const u32 bytes = elements * nsrc * size;
                    const bool plain_copy = nsrc == ndst &&
                                            std::equal(src.begin(), src.begin() + bytes,
                                                       dst.begin());
                    auto engine_dst = before;
                    Tegra::Engines::ApplyRemap(r, src, engine_dst, elements);
                    Check(engine_dst == dst, "ApplyRemap matches the reference remap");
                    const u32 bpp = IdentityRemapBytesPerPixel(r);
                    if (bpp != 0) {
                        ++accepted;
                        Check(plain_copy && bpp == nsrc * size,
                              "accepted identity remap is a plain byte copy");
                    } else if (plain_copy) {
                        ++rejected_equal;
                    }
                }
            }
        }
    }
    // Only SRC_X..SRC_W in order for every active component, 4 sizes x 4 counts.
    Check(accepted == 16, "exactly the 16 in-order remaps are accepted");
    std::fprintf(stderr, "remap identity: %u accepted, %u equivalent but rejected\n", accepted,
                 rejected_equal);
}

enum class Layout { Pitch, Block };
// Programs a remapped multi-line (or 1D when lines == 0) transfer of `width` x `lines`
// elements through the method interface.
void LaunchRemapDMA(Tegra::Engines::MaxwellDMA& engine, const Remap& remap, Layout src_layout,
                    Layout dst_layout, u32 width, u32 lines, u32 bh, u32 src_pitch,
                    u32 dst_pitch) {
    using DMA = Tegra::Engines::MaxwellDMA;
    const auto params = [&](u32 offset) {
        Tegra::DMA::Parameters p{};
        p.width = width;
        p.height = lines;
        p.depth = 1;
        p.block_size.height.Assign(bh);
        const auto words = std::bit_cast<std::array<u32, 6>>(p);
        for (u32 i = 0; i < words.size(); ++i)
            engine.CallMethod(offset / 4 + i, words[i], true);
    };
    params(0x728);
    params(0x70C);
    const auto remap_words = std::bit_cast<std::array<u32, 3>>(remap);
    for (u32 i = 0; i < 3; ++i)
        engine.CallMethod(0x700 / 4 + i, remap_words[i], true);
    engine.CallMethod(0x400 / 4, 0, true);
    engine.CallMethod(0x404 / 4, static_cast<u32>(CopyFixture::Source), true);
    engine.CallMethod(0x408 / 4, 0, true);
    engine.CallMethod(0x40C / 4, static_cast<u32>(CopyFixture::Destination), true);
    engine.CallMethod(0x410 / 4, src_pitch, true);
    engine.CallMethod(0x414 / 4, dst_pitch, true);
    engine.CallMethod(0x418 / 4, width, true);
    engine.CallMethod(0x41C / 4, std::max(lines, 1U), true);
    DMA::LaunchDMA launch{};
    launch.multi_line_enable.Assign(lines != 0 ? 1 : 0);
    launch.remap_enable.Assign(1);
    launch.src_memory_layout.Assign(src_layout == Layout::Pitch ? DMA::LaunchDMA::MemoryLayout::PITCH
                                                                : DMA::LaunchDMA::MemoryLayout::BLOCKLINEAR);
    launch.dst_memory_layout.Assign(dst_layout == Layout::Pitch ? DMA::LaunchDMA::MemoryLayout::PITCH
                                                                : DMA::LaunchDMA::MemoryLayout::BLOCKLINEAR);
    launch.data_transfer_type.Assign(DMA::LaunchDMA::DataTransferType::NON_PIPELINED);
    engine.CallMethod(0x300 / 4, std::bit_cast<u32>(launch), true);
}

// Runs real remapped transfers and compares the whole destination allocation with
// the reference remap applied to the deswizzled surfaces.
void RemapEngineTests(CopyFixture& f) {
    using S = Remap::Swizzle;
    constexpr u32 X = 0, Y = 1, Z = 2, W = 3, A = 4, B = 5, N = 6;
    struct Case {
        u32 size, nsrc, ndst;
        std::array<u32, 4> sw;
    };
    const std::array cases{
        Case{1, 4, 4, {X, Y, Z, W}}, // identity RGBA8
        Case{4, 1, 1, {X, 0, 0, 0}}, // identity R32
        Case{2, 2, 2, {X, Y, 0, 0}}, // identity RG16
        Case{1, 4, 4, {Z, Y, X, W}}, // RGBA8 <-> BGRA8
        Case{2, 2, 2, {Y, X, 0, 0}}, // swap RG16
        Case{1, 3, 4, {X, Y, Z, A}}, // RGB8 -> RGBA8 with constant alpha
        Case{2, 4, 1, {Z, 0, 0, 0}}, // extract one component
        Case{4, 2, 2, {X, N, 0, 0}}, // keep the destination Y
        Case{4, 4, 4, {B, B, B, B}}, // constant fill
        Case{1, 2, 2, {A, B, 0, 0}}, // two constants, 8-bit
    };
    Tegra::Engines::MaxwellDMA engine{f.gpu};
    engine.BindRasterizer(&f.rasterizer);
    u8* const input = f.physical.bytes.data() + CopyFixture::Source;
    u8* const output = f.physical.bytes.data() + CopyFixture::Destination;
    for (const Case& c : cases) {
        const Remap remap = MakeRemap(c.size, c.nsrc, c.ndst, c.sw);
        const u32 src_bpp = c.nsrc * c.size, dst_bpp = c.ndst * c.size;
        const bool identity = Tegra::Engines::IdentityRemapBytesPerPixel(remap) != 0;
        for (Layout src_layout : {Layout::Pitch, Layout::Block}) {
            for (Layout dst_layout : {Layout::Pitch, Layout::Block}) {
                for (u32 width : {16U, 37U}) {
                    for (u32 lines : {0U, 1U, 24U, 32U}) {
                        if (lines == 0 && (src_layout != Layout::Pitch || dst_layout != Layout::Pitch))
                            continue;
                        constexpr u32 bh = 1;
                        const u32 h = std::max(lines, 1U);
                        const u32 src_pitch = width * src_bpp + 16, dst_pitch = width * dst_bpp + 8;
                        const auto size = [&](Layout layout, u32 bpp, u32 pitch) {
                            return layout == Layout::Pitch
                                       ? size_t{pitch} * h
                                       : Tegra::Texture::CalculateSize(true, bpp, width, h, 1, bh, 0);
                        };
                        const size_t src_size = size(src_layout, src_bpp, src_pitch);
                        const size_t dst_size = size(dst_layout, dst_bpp, dst_pitch);
                        for (size_t i = 0; i < src_size; ++i)
                            input[i] = static_cast<u8>(i * 29 + i / 251 + 3);
                        for (size_t i = 0; i < dst_size; ++i)
                            output[i] = static_cast<u8>(i * 113 + 71);
                        // Reference: packed elements in, remap, packed elements out.
                        const u32 elements = width * h;
                        std::vector<u8> src_lin(size_t{elements} * src_bpp),
                            dst_lin(size_t{elements} * dst_bpp);
                        std::vector<u8> expected(output, output + dst_size);
                        if (src_layout == Layout::Pitch) {
                            for (u32 y = 0; y < h; ++y)
                                std::memcpy(src_lin.data() + size_t{y} * width * src_bpp,
                                            input + size_t{y} * (lines ? src_pitch : 0),
                                            size_t{width} * src_bpp);
                        } else {
                            Tegra::Texture::UnswizzleSubrect(src_lin, {input, src_size}, src_bpp,
                                                             width, h, 1, 0, 0, width, h, bh, 0,
                                                             width * src_bpp);
                        }
                        if (dst_layout == Layout::Pitch) {
                            for (u32 y = 0; y < h; ++y)
                                std::memcpy(dst_lin.data() + size_t{y} * width * dst_bpp,
                                            output + size_t{y} * dst_pitch, size_t{width} * dst_bpp);
                        } else {
                            Tegra::Texture::UnswizzleSubrect(dst_lin, {output, dst_size}, dst_bpp,
                                                             width, h, 1, 0, 0, width, h, bh, 0,
                                                             width * dst_bpp);
                        }
                        ReferenceRemap(remap, src_lin, dst_lin, elements);
                        if (dst_layout == Layout::Pitch) {
                            for (u32 y = 0; y < h; ++y)
                                std::memcpy(expected.data() + size_t{y} * dst_pitch,
                                            dst_lin.data() + size_t{y} * width * dst_bpp,
                                            size_t{width} * dst_bpp);
                        } else {
                            Tegra::Texture::SwizzleSubrect(expected, dst_lin, dst_bpp, width, h, 1,
                                                           0, 0, width, h, bh, 0, width * dst_bpp);
                        }
                        const auto attempts = f.rasterizer.dma.image_attempts;
                        LaunchRemapDMA(engine, remap, src_layout, dst_layout, width, lines, bh,
                                       src_pitch, dst_pitch);
                        f.gpu.FlushCaching();
                        char message[160];
                        std::snprintf(message, sizeof(message),
                                      "remapped DMA matches reference (size %u, %u->%u, src %s, "
                                      "dst %s, %ux%u)",
                                      c.size, c.nsrc, c.ndst,
                                      src_layout == Layout::Pitch ? "pitch" : "block",
                                      dst_layout == Layout::Pitch ? "pitch" : "block", width, lines);
                        Check(std::equal(expected.begin(), expected.end(), output), message);
                        if (src_layout == Layout::Block && dst_layout == Layout::Block) {
                            Check((f.rasterizer.dma.image_attempts != attempts) == identity,
                                  "only identity remaps try the image-to-image fast path");
                        }
                    }
                }
            }
        }
    }
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
void LaunchSingleLineDMA(Tegra::Engines::MaxwellDMA& engine, GPUVAddr source,
                         GPUVAddr destination, u32 length, bool semaphore = false) {
    using DMA = Tegra::Engines::MaxwellDMA;
    engine.CallMethod(0x400 / 4, static_cast<u32>(source >> 32), true);
    engine.CallMethod(0x404 / 4, static_cast<u32>(source), true);
    engine.CallMethod(0x408 / 4, static_cast<u32>(destination >> 32), true);
    engine.CallMethod(0x40C / 4, static_cast<u32>(destination), true);
    engine.CallMethod(0x418 / 4, length, true);
    DMA::LaunchDMA launch{};
    launch.data_transfer_type.Assign(DMA::LaunchDMA::DataTransferType::NON_PIPELINED);
    launch.semaphore_type.Assign(semaphore ? DMA::LaunchDMA::SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE
                                          : DMA::LaunchDMA::SemaphoreType::NONE);
    engine.CallMethod(0x300 / 4, std::bit_cast<u32>(launch), true);
}

// Coordinate-based GOB oracle, independent of the production address bit permutation.
size_t GobOffset(size_t offset) {
    const size_t x = offset % 64;
    const size_t y = (offset % 512) / 64;
    return (offset / 512) * 512 + (y / 2) * 64 + (y % 2) * 16 +
           (x / 32) * 256 + ((x % 32) / 16) * 32 + x % 16;
}

void GobEngineTests(CopyFixture& f) {
    struct Observer : TestRasterizer {
        u64 read_bytes{}, queries{};
        void FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
            read_bytes += size;
            TestRasterizer::FlushRegion(addr, size, which);
        }
        void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags,
                   u32, u32) override {
            ++queries;
        }
    } observer;
    f.gpu.FlushCaching();
    f.gpu.BindRasterizer(&observer);
    f.device.BindInterface(&observer);
    Tegra::Engines::MaxwellDMA engine{f.gpu};
    engine.BindRasterizer(&observer);
    constexpr size_t size = 8192;
    std::array<u8, size> input{}, initial{}, expected{}, actual{}, source_after{};
    for (size_t i = 0; i < size; ++i) {
        input[i] = static_cast<u8>(i * 37 + i / 97 + (i / 256) * 11);
    }
    initial.fill(0xCD);
    std::vector<u32> gob_offsets;
    for (u32 offset = 0; offset < 32; ++offset) {
        gob_offsets.push_back(offset);
    }
    for (u32 offset : {127U, 255U, 495U, 511U, 512U, 4093U, 4095U, 4096U}) {
        gob_offsets.push_back(offset);
    }
    constexpr std::array lengths{0U, 1U, 2U, 7U, 15U, 16U, 17U, 23U, 31U, 32U, 33U,
                                 47U, 48U, 49U, 63U, 64U, 65U, 127U, 128U, 129U,
                                 255U, 256U, 257U, 511U, 512U, 513U};
    for (bool fragmented : {false, true}) {
        const GPUVAddr source = fragmented ? 0x6000000 : CopyFixture::Source;
        const GPUVAddr destination = fragmented ? 0x7000000 : CopyFixture::Destination;
        for (bool src_pitch : {false, true}) {
            for (auto pitch_kind : {Tegra::PTEKind::PITCH, Tegra::PTEKind::PITCH_NO_SWIZZLE}) {
                const auto src_kind = src_pitch ? pitch_kind : Tegra::PTEKind::GENERIC_16BX2;
                const auto dst_kind = src_pitch ? Tegra::PTEKind::GENERIC_16BX2 : pitch_kind;
                if (fragmented) {
                    // Small GPU pages backed by nonadjacent device-memory pages.
                    for (u32 page = 0; page < 2; ++page) {
                        f.gpu.Map(source + page * 4096, CopyFixture::Source + page * 8192,
                                  4096, src_kind, false);
                        f.gpu.Map(destination + page * 4096,
                                  CopyFixture::Destination + page * 8192, 4096, dst_kind, false);
                    }
                } else {
                    f.gpu.Map(source, CopyFixture::Source, CopyFixture::RegionSize, src_kind);
                    f.gpu.Map(destination, CopyFixture::Destination, CopyFixture::RegionSize,
                              dst_kind);
                }
                f.gpu.WriteBlockUnsafe(source, input.data(), size);
                for (u32 gob_offset : gob_offsets) {
                    for (u32 pitch_offset : {0U, 1U, 7U, 15U, 31U, 4093U, 4095U}) {
                        for (u32 length : lengths) {
                            const u32 src_offset = src_pitch ? pitch_offset : gob_offset;
                            const u32 dst_offset = src_pitch ? gob_offset : pitch_offset;
                            expected = initial;
                            for (u32 i = 0; i < length; ++i) {
                                const size_t src = src_pitch ? src_offset + i : GobOffset(src_offset + i);
                                const size_t dst = src_pitch ? GobOffset(dst_offset + i) : dst_offset + i;
                                expected[dst] = input[src];
                            }
                            f.gpu.WriteBlockUnsafe(destination, initial.data(), size);
                            observer.read_bytes = 0;
                            observer.queries = 0;
                            LaunchSingleLineDMA(engine, source + src_offset, destination + dst_offset,
                                                length, length == 0);
                            Check(observer.read_bytes == length, "GOB copy reads exactly the requested bytes");
                            Check(observer.queries == (length == 0 ? 1U : 0U),
                                  "zero-length GOB copy still releases its semaphore");
                            f.gpu.FlushCaching();
                            f.gpu.ReadBlockUnsafe(destination, actual.data(), size);
                            Check(actual == expected, "GOB copy matches byte oracle and preserves destination guards");
                            f.gpu.ReadBlockUnsafe(source, source_after.data(), size);
                            Check(source_after == input, "GOB copy preserves source bytes");
                        }
                    }
                }
            }
        }
        if (fragmented) {
            f.gpu.Unmap(source, size);
            f.gpu.Unmap(destination, size);
        }
    }
    f.gpu.BindRasterizer(&f.rasterizer);
    f.device.BindInterface(&f.rasterizer);
    f.gpu.Map(CopyFixture::Source, CopyFixture::Source, CopyFixture::RegionSize);
    f.gpu.Map(CopyFixture::Destination, CopyFixture::Destination, CopyFixture::RegionSize);
}

void GobBenchmark(CopyFixture& f, bool aligned_only, bool same_kind = false) {
    Tegra::Engines::MaxwellDMA engine{f.gpu};
    engine.BindRasterizer(&f.rasterizer);
    for (size_t i = 0; i < 2 * 1024 * 1024; ++i) {
        f.physical.bytes[CopyFixture::Source + i] = static_cast<u8>(i * 37 + i / 97);
    }
    std::puts("direction,gob_offset,pitch_offset,length,median_us,p95_us");
    for (bool src_pitch : {false, true}) {
        f.gpu.Map(CopyFixture::Source, CopyFixture::Source, CopyFixture::RegionSize,
                  src_pitch ? Tegra::PTEKind::PITCH : Tegra::PTEKind::GENERIC_16BX2);
        f.gpu.Map(CopyFixture::Destination, CopyFixture::Destination, CopyFixture::RegionSize,
                  (src_pitch != same_kind) ? Tegra::PTEKind::GENERIC_16BX2 : Tegra::PTEKind::PITCH);
        for (auto offsets : {std::array<u32, 2>{0, 0}, {1, 7}, {15, 1}, {4095, 4093}}) {
            if (aligned_only && offsets[0] != 0) {
                continue;
            }
            for (u32 length : {0U, 15U, 16U, 17U, 31U, 33U, 64U, 256U, 4095U, 4096U,
                               4097U, 65536U, 1048576U}) {
                if (aligned_only && length % 16 != 0) {
                    continue;
                }
                const GPUVAddr source = CopyFixture::Source + (src_pitch ? offsets[1] : offsets[0]);
                const GPUVAddr destination = CopyFixture::Destination + (src_pitch ? offsets[0] : offsets[1]);
                const auto result = Time([&] {
                    LaunchSingleLineDMA(engine, source, destination, length);
                    f.gpu.FlushCaching();
                }, 31);
                const char* direction = same_kind ? (src_pitch ? "pitch_to_pitch" : "gob_to_gob")
                                                  : (src_pitch ? "pitch_to_gob" : "gob_to_pitch");
                std::printf("%s,%u,%u,%u,%.3f,%.3f\n", direction,
                            offsets[0], offsets[1], length, result.median, result.p95);
            }
        }
    }
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
    // Remapped block copies. An identity remap previously took the software route
    // (reproduced with a dirty report); a real swizzle now pays the component-wise
    // route, compared with that same software identity cost.
    for (auto dims : {std::array<u32, 2>{256, 256}, {1280, 768}, {1920, 1088}}) {
        struct Kind {
            const char* name;
            Remap remap;
        };
        const std::array kinds{
            Kind{"dma_remap_identity_rgba8", MakeRemap(1, 4, 4, {0, 1, 2, 3})},
            Kind{"dma_remap_identity_r32", MakeRemap(4, 1, 1, {0, 0, 0, 0})},
            Kind{"dma_remap_bgra8_vs_identity", MakeRemap(1, 4, 4, {2, 1, 0, 3})},
            Kind{"dma_remap_rgb8_rgba8_vs_identity", MakeRemap(1, 3, 4, {0, 1, 2, 4})},
        };
        for (const Kind& kind : kinds) {
            const Remap identity = MakeRemap(kind.remap.component_size_minus_one + 1,
                                             kind.remap.num_dst_components_minus_one + 1,
                                             kind.remap.num_dst_components_minus_one + 1,
                                             {0, 1, 2, 3});
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                f.rasterizer.dirty = true;
                const auto before = Time([&] {
                    LaunchRemapDMA(dma, identity, Layout::Block, Layout::Block, dims[0], dims[1],
                                   3, 0, 0);
                    f.gpu.FlushCaching();
                });
                f.rasterizer.dirty = false;
                const auto after = Time([&] {
                    LaunchRemapDMA(dma, kind.remap, Layout::Block, Layout::Block, dims[0],
                                   dims[1], 3, 0, 0);
                    f.gpu.FlushCaching();
                });
                std::printf("%s,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.2f\n", kind.name, dims[0],
                            dims[1], Tegra::Engines::RemapDstBytesPerPixel(kind.remap), repeat,
                            before.median, after.median, before.p95, after.p95,
                            before.median / after.median);
            }
        }
    }
    // The previous remap route deswizzled and reswizzled 4-byte elements and
    // ignored the swizzle. Time it, and the component shuffle alone.
    for (auto dims : {std::array<u32, 2>{256, 256}, {1920, 1088}}) {
        const u32 w = dims[0], h = dims[1];
        const size_t bytes = Tegra::Texture::CalculateSize(true, 4, w, h, 1, 3, 0);
        std::vector<u8> lin(size_t{w} * h * 4), out(lin.size());
        std::span<u8> in_mem{f.physical.bytes.data() + CopyFixture::Source, bytes};
        std::span<u8> out_mem{f.physical.bytes.data() + CopyFixture::Destination, bytes};
        const auto old_route = Time([&] {
            Tegra::Texture::UnswizzleSubrect(lin, in_mem, 4, w, h, 1, 0, 0, w, h, 3, 0, w * 4);
            Tegra::Texture::SwizzleSubrect(out_mem, lin, 4, w, h, 1, 0, 0, w, h, 3, 0, w * 4);
        });
        const Remap bgra = MakeRemap(1, 4, 4, {2, 1, 0, 3});
        const auto shuffle =
            Time([&] { Tegra::Engines::ApplyRemap(bgra, lin, out, size_t{w} * h); });
        std::printf("remap_parts,%u,%u,4,0,%.3f,%.3f,%.3f,%.3f,0\n", w, h, old_route.median,
                    shuffle.median, old_route.p95, shuffle.p95);
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
    else if (argc > 1 && std::string_view(argv[1]) == "--benchmark-gob")
        GobBenchmark(fixture, false);
    else if (argc > 1 && std::string_view(argv[1]) == "--benchmark-gob-aligned")
        GobBenchmark(fixture, true);
    else if (argc > 1 && std::string_view(argv[1]) == "--benchmark-gob-control")
        GobBenchmark(fixture, true, true);
    else if (argc > 1 && std::string_view(argv[1]) == "--gob-only")
        GobEngineTests(fixture);
    else {
        LayoutTests(fixture);
        MappingTests(fixture);
        DmaTests();
        RemapIdentityTests();
        RemapEngineTests(fixture);
        DmaEngineTests(fixture);
        GobEngineTests(fixture);
    }
    std::fprintf(stderr, "%u checks, %u failures\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
