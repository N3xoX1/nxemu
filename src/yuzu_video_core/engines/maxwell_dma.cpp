// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>
#include <optional>
#include <span>

#include "yuzu_common/algorithm.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/engines/dma_remap.h"
#include "yuzu_video_core/engines/fast_copy.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/textures/decoders.h"

namespace Tegra::Engines {

using namespace Texture;

// Expresses an identity remap in bytes, the unit of non-remapped transfers, for
// the lifetime of the object. Active() is false when the scaled values do not fit.
class IdentityRemapAsBytes {
public:
    explicit IdentityRemapAsBytes(MaxwellDMA::Regs& regs_)
        : regs{regs_}, line_length_in{regs.line_length_in}, src_params{regs.src_params},
          dst_params{regs.dst_params} {
        const u32 bpp = IdentityRemapBytesPerPixel(regs.remap_const);
        if (bpp == 0) {
            return;
        }
        u32 length = 0;
        DMA::Parameters src = src_params;
        DMA::Parameters dst = dst_params;
        if (!Scale(line_length_in, bpp, length) || !Scale(src, bpp) || !Scale(dst, bpp)) {
            return;
        }
        regs.line_length_in = length;
        regs.src_params = src;
        regs.dst_params = dst;
        regs.launch_dma.remap_enable.Assign(0);
        active = true;
    }

    ~IdentityRemapAsBytes() {
        if (active) {
            regs.line_length_in = line_length_in;
            regs.src_params = src_params;
            regs.dst_params = dst_params;
            regs.launch_dma.remap_enable.Assign(1);
        }
    }

    IdentityRemapAsBytes(const IdentityRemapAsBytes&) = delete;
    IdentityRemapAsBytes& operator=(const IdentityRemapAsBytes&) = delete;

    bool Active() const {
        return active;
    }

private:
    static bool Scale(u32 value, u32 bpp, u32& out) {
        const u64 scaled = u64{value} * bpp;
        out = static_cast<u32>(scaled);
        return scaled <= std::numeric_limits<u32>::max();
    }

    static bool Scale(DMA::Parameters& params, u32 bpp) {
        u32 width = 0;
        const u64 origin_x = u64{params.origin.x} * bpp;
        if (!Scale(params.width, bpp, width) || origin_x > 0xFFFF) {
            return false;
        }
        params.width = width;
        params.origin.x.Assign(static_cast<u32>(origin_x));
        return true;
    }

    MaxwellDMA::Regs& regs;
    const u32 line_length_in;
    const DMA::Parameters src_params;
    const DMA::Parameters dst_params;
    bool active = false;
};

namespace {

GPUVAddr LineAddress(GPUVAddr base, s32 pitch, u32 line) {
    return base + static_cast<u64>(static_cast<s64>(pitch) * line);
}

} // Anonymous namespace

MaxwellDMA::MaxwellDMA(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_} {
    execution_mask.reset();
    execution_mask[offsetof(Regs, launch_dma) / sizeof(u32)] = true;
}

MaxwellDMA::~MaxwellDMA() = default;

void MaxwellDMA::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void MaxwellDMA::ConsumeSinkImpl() {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void MaxwellDMA::CallMethod(u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < NUM_REGS, "Invalid MaxwellDMA register");

    regs.reg_array[method] = method_argument;

    if (method == offsetof(Regs, launch_dma) / sizeof(u32)) {
        Launch();
    }
}

void MaxwellDMA::CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                 u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(method, base_start[i], methods_pending - i <= 1);
    }
}

void MaxwellDMA::Launch() {
    LOG_TRACE(Render_OpenGL, "DMA copy 0x{:x} -> 0x{:x}", static_cast<GPUVAddr>(regs.offset_in),
              static_cast<GPUVAddr>(regs.offset_out));

    // TODO(Subv): Perform more research and implement all features of this engine.
    const LaunchDMA& launch = regs.launch_dma;
    ASSERT(launch.interrupt_type == LaunchDMA::InterruptType::NONE);
    ASSERT(launch.data_transfer_type == LaunchDMA::DataTransferType::NON_PIPELINED);

    // Identity remaps run as the plain byte copies they are; any other remap
    // takes the component-wise path.
    std::optional<IdentityRemapAsBytes> identity_remap;
    if (launch.remap_enable != 0) {
        identity_remap.emplace(regs);
        if (!identity_remap->Active()) {
            if (launch.multi_line_enable) {
                memory_manager.FlushCaching();
                CopyRemapped();
            } else {
                CopyRemappedLinear();
            }
            ReleaseSemaphore();
            return;
        }
    }

    if (launch.multi_line_enable) {
        const bool is_src_pitch = launch.src_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        const bool is_dst_pitch = launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        memory_manager.FlushCaching();
        if (!is_src_pitch && !is_dst_pitch) {
            // If both the source and the destination are in block layout, assert.
            CopyBlockLinearToBlockLinear();
            ReleaseSemaphore();
            return;
        }

        if (is_src_pitch && is_dst_pitch) {
            for (u32 line = 0; line < regs.line_count; ++line) {
                const GPUVAddr source_line =
                    regs.offset_in + static_cast<size_t>(line) * regs.pitch_in;
                const GPUVAddr dest_line =
                    regs.offset_out + static_cast<size_t>(line) * regs.pitch_out;
                memory_manager.CopyBlock(dest_line, source_line, regs.line_length_in);
            }
        } else {
            if (!is_src_pitch && is_dst_pitch) {
                CopyBlockLinearToPitch();
            } else {
                CopyPitchToBlockLinear();
            }
        }
    } else {
        auto& accelerate = rasterizer->AccessAccelerateDMA();
        memory_manager.FlushCaching();
        const auto convert_linear_2_blocklinear_addr = [](u64 address) {
            return (address & ~0x1f0ULL) | ((address & 0x40) >> 2) | ((address & 0x10) << 1) |
                   ((address & 0x180) >> 1) | ((address & 0x20) << 3);
        };
        const auto src_kind = memory_manager.GetPageKind(regs.offset_in);
        const auto dst_kind = memory_manager.GetPageKind(regs.offset_out);
        const bool is_src_pitch = IsPitchKind(src_kind);
        const bool is_dst_pitch = IsPitchKind(dst_kind);
        if (!is_src_pitch && is_dst_pitch) {
            UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
            UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
            UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
            read_buffer.resize_destructive(16);
            for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                Tegra::Memory::GpuGuestMemoryScoped<
                    u8, GuestMemoryFlags::SafeReadCachedWrite>
                    tmp_write_buffer(memory_manager,
                                     convert_linear_2_blocklinear_addr(regs.offset_in + offset),
                                     16, &read_buffer);
                tmp_write_buffer.SetAddressAndSize(regs.offset_out + offset, 16);
            }
        } else if (is_src_pitch && !is_dst_pitch) {
            UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
            UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
            UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
            read_buffer.resize_destructive(16);
            for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                Tegra::Memory::GpuGuestMemoryScoped<
                    u8, GuestMemoryFlags::SafeReadCachedWrite>
                    tmp_write_buffer(memory_manager, regs.offset_in + offset, 16, &read_buffer);
                tmp_write_buffer.SetAddressAndSize(
                    convert_linear_2_blocklinear_addr(regs.offset_out + offset), 16);
            }
        } else {
            if (!accelerate.BufferCopy(regs.offset_in, regs.offset_out, regs.line_length_in)) {
                // Snapshot the source before writing. GPU virtual ranges can alias physical
                // memory, and a direct guest span would otherwise memcpy overlapping bytes.
                read_buffer.resize_destructive(regs.line_length_in);
                memory_manager.ReadBlock(regs.offset_in, read_buffer.data(), regs.line_length_in);
                memory_manager.WriteBlockCached(regs.offset_out, read_buffer.data(),
                                                regs.line_length_in);
            }
        }
    }

    ReleaseSemaphore();
}

void MaxwellDMA::CopyBlockLinearToPitch() {
    UNIMPLEMENTED_IF(regs.launch_dma.remap_enable != 0);

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand src_operand;
    src_operand.bytes_per_pixel = bytes_per_pixel;
    src_operand.params = regs.src_params;
    src_operand.address = regs.offset_in;

    DMA::BufferOperand dst_operand;
    dst_operand.pitch = static_cast<u32>(std::abs(regs.pitch_out));
    dst_operand.width = regs.line_length_in;
    dst_operand.height = regs.line_count;
    dst_operand.address = regs.offset_out;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.ImageToBuffer(copy_info, src_operand, dst_operand)) {
        return;
    }

    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth == 0 && regs.src_params.depth != 1);

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src_params = regs.src_params;

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = src_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = src_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_in));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = src_params.height;
    const u32 depth = src_params.depth;
    const u32 block_height = src_params.block_size.height;
    const u32 block_depth = src_params.block_size.depth;
    const size_t src_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);

    const size_t dst_size = dst_operand.pitch * regs.line_count;

    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, src_operand.address, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::UnsafeReadCachedWrite>
        tmp_write_buffer(memory_manager, dst_operand.address, dst_size, &write_buffer);

    UnswizzleSubrect(tmp_write_buffer, tmp_read_buffer, bytes_per_pixel, width, height, depth,
                     x_offset, src_params.origin.y, x_elements, regs.line_count, block_height,
                     block_depth, dst_operand.pitch);
}

void MaxwellDMA::CopyPitchToBlockLinear() {
    UNIMPLEMENTED_IF_MSG(regs.dst_params.block_size.width != 0, "Block width is not one");
    UNIMPLEMENTED_IF(regs.dst_params.layer != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;
    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand dst_operand;
    dst_operand.bytes_per_pixel = bytes_per_pixel;
    dst_operand.params = regs.dst_params;
    dst_operand.address = regs.offset_out;
    DMA::BufferOperand src_operand;
    src_operand.pitch = regs.pitch_in;
    src_operand.width = regs.line_length_in;
    src_operand.height = regs.line_count;
    src_operand.address = regs.offset_in;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.BufferToImage(copy_info, src_operand, dst_operand)) {
        return;
    }

    const auto& dst_params = regs.dst_params;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = dst_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = dst_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_out));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = dst_params.height;
    const u32 depth = dst_params.depth;
    const u32 block_height = dst_params.block_size.height;
    const u32 block_depth = dst_params.block_size.depth;
    const size_t dst_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);
    const size_t src_size = static_cast<size_t>(regs.pitch_in) * regs.line_count;

    GPUVAddr src_addr = regs.offset_in;
    GPUVAddr dst_addr = regs.offset_out;
    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, src_addr, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::UnsafeReadCachedWrite>
        tmp_write_buffer(memory_manager, dst_addr, dst_size, &write_buffer);

    //  If the input is linear and the output is tiled, swizzle the input and copy it over.
    SwizzleSubrect(tmp_write_buffer, tmp_read_buffer, bytes_per_pixel, width, height, depth,
                   x_offset, dst_params.origin.y, x_elements, regs.line_count, block_height,
                   block_depth, regs.pitch_in);
}

void MaxwellDMA::CopyBlockLinearToBlockLinear() {
    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    if (!is_remapping) {
        const DMA::ImageCopy copy{.length_x = regs.line_length_in, .length_y = regs.line_count};
        const DMA::ImageOperand src_operand{
            .bytes_per_pixel = 1, .params = regs.src_params, .address = regs.offset_in};
        const DMA::ImageOperand dst_operand{
            .bytes_per_pixel = 1, .params = regs.dst_params, .address = regs.offset_out};
        if (rasterizer->AccessAccelerateDMA().ImageToImage(memory_manager, copy, src_operand,
                                                           dst_operand)) {
            return;
        }
        // Identical full guest layouts need no intermediate linear image. Only
        // copy allocation bytes when there is no GOB padding to preserve.
        const size_t size = DmaImageCopySize(copy, src_operand, dst_operand);
        if (size != 0 && regs.src_params.width % 64 == 0 &&
            regs.src_params.height % (8U << static_cast<u32>(regs.src_params.block_size.height)) ==
                0 &&
            TryFastGuestCopy(memory_manager, regs.offset_out, regs.offset_in, size)) {
            return;
        }
    }

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src = regs.src_params;
    const DMA::Parameters& dst = regs.dst_params;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 src_width = src.width;
    u32 dst_width = dst.width;
    u32 x_elements = regs.line_length_in;
    u32 src_x_offset = src.origin.x;
    u32 dst_x_offset = dst.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            src_width, dst_width, x_elements, src_x_offset, dst_x_offset,
            static_cast<u32>(regs.offset_in), static_cast<u32>(regs.offset_out));
        src_width >>= bpp_shift;
        dst_width >>= bpp_shift;
        x_elements >>= bpp_shift;
        src_x_offset >>= bpp_shift;
        dst_x_offset >>= bpp_shift;
    }

    const u32 bytes_per_pixel = base_bpp << bpp_shift;
    const size_t src_size = CalculateSize(true, bytes_per_pixel, src_width, src.height, src.depth,
                                          src.block_size.height, src.block_size.depth);
    const size_t dst_size = CalculateSize(true, bytes_per_pixel, dst_width, dst.height, dst.depth,
                                          dst.block_size.height, dst.block_size.depth);

    const u32 pitch = x_elements * bytes_per_pixel;
    const size_t mid_buffer_size = pitch * regs.line_count;

    intermediate_buffer.resize_destructive(mid_buffer_size);

    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, regs.offset_in, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadCachedWrite>
        tmp_write_buffer(memory_manager, regs.offset_out, dst_size, &write_buffer);

    UnswizzleSubrect(intermediate_buffer, tmp_read_buffer, bytes_per_pixel, src_width, src.height,
                     src.depth, src_x_offset, src.origin.y, x_elements, regs.line_count,
                     src.block_size.height, src.block_size.depth, pitch);

    SwizzleSubrect(tmp_write_buffer, intermediate_buffer, bytes_per_pixel, dst_width, dst.height,
                   dst.depth, dst_x_offset, dst.origin.y, x_elements, regs.line_count,
                   dst.block_size.height, dst.block_size.depth, pitch);
}

void MaxwellDMA::CopyRemappedLinear() {
    const RemapConst& remap = regs.remap_const;
    const u32 dst_components = remap.num_dst_components_minus_one + 1;
    const size_t elements = regs.line_length_in;
    const size_t src_bytes = elements * RemapSrcBytesPerPixel(remap);
    const size_t dst_bytes = elements * RemapDstBytesPerPixel(remap);
    const auto swizzle = RemapSwizzle(remap);

    // Filling every component with the same 32-bit constant is a buffer clear.
    bool uniform = remap.component_size_minus_one == 3 &&
                   (swizzle[0] == RemapConst::Swizzle::CONST_A ||
                    swizzle[0] == RemapConst::Swizzle::CONST_B);
    for (u32 i = 1; i < dst_components; ++i) {
        uniform &= swizzle[i] == swizzle[0];
    }
    if (uniform) {
        const u32 value = swizzle[0] == RemapConst::Swizzle::CONST_A ? remap.remap_consta_value
                                                                     : remap.remap_constb_value;
        const size_t words = elements * dst_components;
        rasterizer->AccessAccelerateDMA().BufferClear(regs.offset_out, words, value);
        write_buffer.resize_destructive(dst_bytes);
        std::ranges::fill(std::span<u32>(reinterpret_cast<u32*>(write_buffer.data()), words),
                          value);
        memory_manager.WriteBlockUnsafe(regs.offset_out, write_buffer.data(), dst_bytes);
        return;
    }

    read_buffer.resize_destructive(src_bytes);
    if (RemapReadsSource(remap)) {
        memory_manager.ReadBlock(regs.offset_in, read_buffer.data(), src_bytes);
    }
    write_buffer.resize_destructive(dst_bytes);
    if (RemapKeepsDestination(remap)) {
        memory_manager.ReadBlock(regs.offset_out, write_buffer.data(), dst_bytes);
    }
    ApplyRemap(remap, {read_buffer.data(), src_bytes}, {write_buffer.data(), dst_bytes}, elements);
    memory_manager.WriteBlock(regs.offset_out, write_buffer.data(), dst_bytes);
}

void MaxwellDMA::CopyRemapped() {
    const RemapConst& remap = regs.remap_const;
    const LaunchDMA& launch = regs.launch_dma;
    const bool is_src_pitch = launch.src_memory_layout == LaunchDMA::MemoryLayout::PITCH;
    const bool is_dst_pitch = launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH;
    const u32 src_bpp = RemapSrcBytesPerPixel(remap);
    const u32 dst_bpp = RemapDstBytesPerPixel(remap);
    const u32 width = regs.line_length_in;
    const u32 height = regs.line_count;
    const size_t elements = size_t{width} * height;
    const u32 src_pitch = width * src_bpp;
    const u32 dst_pitch = width * dst_bpp;

    // Equal full block-linear layouts: swizzling only moves whole elements, so the
    // byte permutation can run on the guest allocations directly.
    if (TryRemapInPlace()) {
        return;
    }

    // Gather the source elements into a packed linear image.
    intermediate_buffer.resize_destructive(elements * src_bpp);
    if (RemapReadsSource(remap)) {
        if (is_src_pitch) {
            for (u32 line = 0; line < height; ++line) {
                memory_manager.ReadBlock(LineAddress(regs.offset_in, regs.pitch_in, line),
                                         intermediate_buffer.data() + size_t{line} * src_pitch,
                                         src_pitch);
            }
        } else {
            const DMA::Parameters& src = regs.src_params;
            const size_t src_size =
                CalculateSize(true, src_bpp, src.width, src.height, src.depth,
                              src.block_size.height, src.block_size.depth);
            Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::SafeRead> tmp_read_buffer(
                memory_manager, regs.offset_in, src_size, &read_buffer);
            UnswizzleSubrect(intermediate_buffer, tmp_read_buffer, src_bpp, src.width, src.height,
                             src.depth, src.origin.x, src.origin.y, width, height,
                             src.block_size.height, src.block_size.depth, src_pitch);
        }
    }
    const std::span<const u8> src_elements{intermediate_buffer.data(), elements * src_bpp};
    const bool keeps_destination = RemapKeepsDestination(remap);

    if (is_dst_pitch) {
        write_buffer.resize_destructive(elements * dst_bpp);
        if (keeps_destination) {
            for (u32 line = 0; line < height; ++line) {
                memory_manager.ReadBlock(LineAddress(regs.offset_out, regs.pitch_out, line),
                                         write_buffer.data() + size_t{line} * dst_pitch,
                                         dst_pitch);
            }
        }
        ApplyRemap(remap, src_elements, {write_buffer.data(), elements * dst_bpp}, elements);
        for (u32 line = 0; line < height; ++line) {
            memory_manager.WriteBlock(LineAddress(regs.offset_out, regs.pitch_out, line),
                                      write_buffer.data() + size_t{line} * dst_pitch, dst_pitch);
        }
        return;
    }

    const DMA::Parameters& dst = regs.dst_params;
    const size_t dst_size = CalculateSize(true, dst_bpp, dst.width, dst.height, dst.depth,
                                          dst.block_size.height, dst.block_size.depth);
    remap_buffer.resize_destructive(elements * dst_bpp);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadCachedWrite>
        tmp_write_buffer(memory_manager, regs.offset_out, dst_size, &write_buffer);
    if (keeps_destination) {
        UnswizzleSubrect(remap_buffer, tmp_write_buffer, dst_bpp, dst.width, dst.height, dst.depth,
                         dst.origin.x, dst.origin.y, width, height, dst.block_size.height,
                         dst.block_size.depth, dst_pitch);
    }
    ApplyRemap(remap, src_elements, {remap_buffer.data(), elements * dst_bpp}, elements);
    SwizzleSubrect(tmp_write_buffer, remap_buffer, dst_bpp, dst.width, dst.height, dst.depth,
                   dst.origin.x, dst.origin.y, width, height, dst.block_size.height,
                   dst.block_size.depth, dst_pitch);
}

bool MaxwellDMA::TryRemapInPlace() {
    const LaunchDMA& launch = regs.launch_dma;
    const DMA::Parameters& src = regs.src_params;
    const DMA::Parameters& dst = regs.dst_params;
    const auto shuffle = MakeRemapShuffle(regs.remap_const);
    if (!shuffle || shuffle->src_bytes_per_pixel != shuffle->dst_bytes_per_pixel ||
        launch.src_memory_layout != LaunchDMA::MemoryLayout::BLOCKLINEAR ||
        launch.dst_memory_layout != LaunchDMA::MemoryLayout::BLOCKLINEAR) {
        return false;
    }
    // Same element size on both sides: the byte layout of the copy is that of a
    // non-remapped transfer of width * bpp bytes.
    const u32 bpp = shuffle->dst_bytes_per_pixel;
    DMA::ImageOperand src_operand{.bytes_per_pixel = 1, .params = src, .address = regs.offset_in};
    DMA::ImageOperand dst_operand{.bytes_per_pixel = 1, .params = dst, .address = regs.offset_out};
    const u64 width_bytes = u64{src.width} * bpp;
    if (width_bytes > std::numeric_limits<u32>::max() || src.width != dst.width ||
        regs.line_length_in != src.width) {
        return false;
    }
    src_operand.params.width = static_cast<u32>(width_bytes);
    dst_operand.params.width = static_cast<u32>(width_bytes);
    const DMA::ImageCopy copy{.length_x = static_cast<u32>(width_bytes),
                              .length_y = regs.line_count};
    const size_t size = DmaImageCopySize(copy, src_operand, dst_operand);
    // GOB padding must stay untouched in the destination.
    if (size == 0 || width_bytes % 64 != 0 ||
        src.height % (8U << static_cast<u32>(src.block_size.height)) != 0) {
        return false;
    }
    const u64 distance = regs.offset_in < regs.offset_out ? regs.offset_out - regs.offset_in
                                                          : regs.offset_in - regs.offset_out;
    if (distance != 0 && distance < size) {
        return false;
    }
    Tegra::Memory::GpuGuestMemory<u8, GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, regs.offset_in, size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadCachedWrite>
        tmp_write_buffer(memory_manager, regs.offset_out, size, &write_buffer);
    // Different GPU addresses can still share host memory.
    const auto src_host = reinterpret_cast<uintptr_t>(tmp_read_buffer.data());
    const auto dst_host = reinterpret_cast<uintptr_t>(tmp_write_buffer.data());
    const auto host_distance = src_host < dst_host ? dst_host - src_host : src_host - dst_host;
    const u8* source = tmp_read_buffer.data();
    if (host_distance != 0 && host_distance < size) {
        remap_buffer.resize_destructive(size);
        std::memcpy(remap_buffer.data(), source, size);
        source = remap_buffer.data();
    }
    ApplyRemapShuffle(*shuffle, source, tmp_write_buffer.data(), size / bpp);
    return true;
}

void MaxwellDMA::ReleaseSemaphore() {
    const auto type = regs.launch_dma.semaphore_type;
    const GPUVAddr address = regs.semaphore.address;
    const u32 payload = regs.semaphore.payload;
    VideoCommon::QueryPropertiesFlags flags{VideoCommon::QueryPropertiesFlags::IsAFence};
    switch (type) {
    case LaunchDMA::SemaphoreType::NONE:
        break;
    case LaunchDMA::SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE: {
        rasterizer->Query(address, VideoCommon::QueryType::Payload, flags, payload, 0);
        break;
    }
    case LaunchDMA::SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE: {
        rasterizer->Query(address, VideoCommon::QueryType::Payload,
                          flags | VideoCommon::QueryPropertiesFlags::HasTimeout, payload, 0);
        break;
    }
    default:
        ASSERT_MSG(false, "Unknown semaphore type: {}", static_cast<u32>(type.Value()));
        break;
    }
}

} // namespace Tegra::Engines
