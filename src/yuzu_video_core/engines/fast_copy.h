// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>

#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/surface.h"

namespace Tegra::Engines {

inline bool TryFastGuestCopy(MemoryManager& memory, GPUVAddr dst, GPUVAddr src, size_t size) {
    if (size == 0 || memory.GetPageKind(src) != memory.GetPageKind(dst) ||
        memory.IsMemoryDirty(src, size) || memory.IsMemoryDirty(dst, size) ||
        memory.MaxContinuousRange(src, size) != size ||
        memory.MaxContinuousRange(dst, size) != size) {
        return false;
    }
    // Device addresses can themselves alias the same physical backing.
    const auto* src_ptr = memory.GetSpan(src, size);
    const auto* dst_ptr = memory.GetSpan(dst, size);
    if (!src_ptr || !dst_ptr) {
        return false;
    }
    const auto src_host = reinterpret_cast<uintptr_t>(src_ptr);
    const auto dst_host = reinterpret_cast<uintptr_t>(dst_ptr);
    const auto distance = src_host < dst_host ? dst_host - src_host : src_host - dst_host;
    if (src != dst && distance < size) {
        return false;
    }
    if (src != dst) {
        memory.CopyBlock(dst, src, size);
    }
    return true;
}

// Zero means the software blitter must handle the transfer. Keep these layout
// checks independent of cache lookups so ineligible copies remain inexpensive.
inline size_t Fast2DCopySize(const Fermi2D::Surface& src, const Fermi2D::Surface& dst,
                             const Fermi2D::Config& config) {
    using VideoCore::Surface::BytesPerBlock;
    using VideoCore::Surface::DefaultBlockHeight;
    using VideoCore::Surface::DefaultBlockWidth;
    using VideoCore::Surface::PixelFormatFromRenderTargetFormat;
    if (config.must_accelerate || config.filter != Fermi2D::Filter::Point ||
        config.operation != Fermi2D::Operation::SrcCopy || src.format != dst.format ||
        src.linear != dst.linear || src.width == 0 || src.height == 0 ||
        src.width > std::numeric_limits<s32>::max() ||
        src.height > std::numeric_limits<s32>::max() || src.width != dst.width ||
        src.height != dst.height || src.depth != 1 || dst.depth != 1 || src.layer != 0 ||
        dst.layer != 0 || config.src_x0 != 0 || config.src_y0 != 0 || config.dst_x0 != 0 ||
        config.dst_y0 != 0 || config.src_x1 != static_cast<s32>(src.width) ||
        config.src_y1 != static_cast<s32>(src.height) ||
        config.dst_x1 != static_cast<s32>(dst.width) ||
        config.dst_y1 != static_cast<s32>(dst.height)) {
        return 0;
    }
    const auto format = PixelFormatFromRenderTargetFormat(src.format);
    const u32 bpp = BytesPerBlock(format);
    if (bpp == 0 || DefaultBlockWidth(format) != 1 || DefaultBlockHeight(format) != 1) {
        return 0;
    }
    const u64 row_bytes = u64{src.width} * bpp;
    const u64 size = row_bytes * src.height;
    // The existing texture decoders and ImageBase store byte sizes in u32.
    if (size > std::numeric_limits<u32>::max()) {
        return 0;
    }
    if (src.linear == Fermi2D::MemoryLayout::Pitch) {
        return src.pitch == dst.pitch && row_bytes == src.pitch ? size : 0;
    }
    if (src.linear != Fermi2D::MemoryLayout::BlockLinear || src.block_width != dst.block_width ||
        src.block_height != dst.block_height || src.block_depth != dst.block_depth ||
        src.block_width != 0 || src.block_depth != 0 || src.block_height > 5 ||
        row_bytes % 64 != 0 || src.height % (8U << static_cast<u32>(src.block_height)) != 0) {
        return 0;
    }
    return size;
}

inline size_t DmaImageCopySize(const DMA::ImageCopy& copy, const DMA::ImageOperand& src,
                               const DMA::ImageOperand& dst) {
    const auto& s = src.params;
    const auto& d = dst.params;
    if (src.bytes_per_pixel != 1 || dst.bytes_per_pixel != 1 || s.depth != 1 || d.depth != 1 ||
        s.layer != 0 || d.layer != 0 || s.origin.x != 0 || s.origin.y != 0 || d.origin.x != 0 ||
        d.origin.y != 0 || s.width != d.width || s.height != d.height ||
        s.block_size.width != d.block_size.width || s.block_size.height != d.block_size.height ||
        s.block_size.depth != d.block_size.depth ||
        s.block_size.gob_height != d.block_size.gob_height || s.block_size.width != 0 ||
        s.block_size.depth != 0 || s.block_size.gob_height != 0 || s.block_size.height > 5 ||
        copy.length_x == 0 || copy.length_y == 0 || copy.length_x != s.width ||
        copy.length_y != s.height || s.width > std::numeric_limits<s32>::max() ||
        s.height > std::numeric_limits<s32>::max()) {
        return 0;
    }
    // Host images contain texels only. Guest readback preserves GOB padding.
    // Widen before alignment/multiplication: CalculateSize uses u32 arithmetic.
    const u64 block_height = 8ULL << static_cast<u32>(s.block_size.height);
    const u64 width = (u64{s.width} + 63) & ~63ULL;
    const u64 height = (u64{s.height} + block_height - 1) & ~(block_height - 1);
    const u64 size = width * height;
    return size <= std::numeric_limits<u32>::max() ? size : 0;
}

} // namespace Tegra::Engines
