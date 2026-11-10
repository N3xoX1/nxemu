// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>

#include "yuzu_common/alignment.h"
#include "yuzu_common/algorithm.h"
#include "yuzu_video_core/engines/engine_upload.h"
#include "yuzu_video_core/guest_memory.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include "yuzu_video_core/textures/decoders.h"

namespace Tegra::Engines::Upload {

State::State(MemoryManager& memory_manager_, Registers& regs_)
    : regs{regs_}, memory_manager{memory_manager_} {}

State::~State() = default;

void State::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void State::ProcessExec(const bool is_linear_) {
    write_offset = 0;
    // Each row in the inline method stream starts on a new word.
    const u64 pitch = Common::AlignUp<u64>(regs.line_length_in, sizeof(u32));
    const u64 size = pitch * regs.line_count;
    copy_size = size <= std::numeric_limits<u32>::max() ? static_cast<u32>(size) : 0;
    is_linear = is_linear_;
}

void State::ProcessData(const u32 data) {
    // An inline transfer can span more than one method packet.
    ProcessData(&data, 1);
}

void State::ProcessData(const u32* data, size_t num_data) {
    const size_t size = std::min<size_t>(num_data * sizeof(u32), copy_size - write_offset);
    if (size == 0) {
        return;
    }
    const auto* bytes = reinterpret_cast<const u8*>(data);
    if (write_offset == 0 && size == copy_size) {
        write_offset = copy_size;
        ProcessData(std::span{bytes, size});
        return;
    }
    if (write_offset == 0) {
        inner_buffer.resize_destructive(copy_size);
    }
    std::memcpy(inner_buffer.data() + write_offset, bytes, size);
    write_offset += static_cast<u32>(size);
    if (write_offset == copy_size) {
        ProcessData(inner_buffer);
    }
}

void State::ProcessData(std::span<const u8> read_buffer) {
    const GPUVAddr address{regs.dest.Address()};
    const u32 source_pitch = Common::AlignUp(regs.line_length_in, sizeof(u32));
    if (is_linear) {
        for (size_t line = 0; line < regs.line_count; ++line) {
            const GPUVAddr dest_line = address + line * regs.dest.pitch;
            std::span<const u8> buffer(read_buffer.data() + line * source_pitch,
                                       regs.line_length_in);
            rasterizer->AccelerateInlineToMemory(dest_line, regs.line_length_in, buffer);
        }
    } else {
        u32 width = regs.dest.width;
        u32 x_elements = regs.line_length_in;
        u32 x_offset = regs.dest.x;
        const u32 bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(address));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
        const u32 bytes_per_pixel = 1U << bpp_shift;
        const std::size_t dst_size = Tegra::Texture::CalculateSize(
            true, bytes_per_pixel, width, regs.dest.height, regs.dest.depth,
            regs.dest.BlockHeight(), regs.dest.BlockDepth());

        Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadCachedWrite>
            tmp(memory_manager, address, dst_size, &tmp_buffer);

        Tegra::Texture::SwizzleSubrect(tmp, read_buffer, bytes_per_pixel, width, regs.dest.height,
                                       regs.dest.depth, x_offset, regs.dest.y, x_elements,
                                       regs.line_count, regs.dest.BlockHeight(),
                                       regs.dest.BlockDepth(), source_pitch);
    }
}

} // namespace Tegra::Engines::Upload
