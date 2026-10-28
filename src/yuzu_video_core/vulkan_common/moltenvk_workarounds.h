// SPDX-License-Identifier: GPL-2.0-or-later

/*
The following notice applies to portions of the duplicate-attachment workaround.

MIT License

Copyright (c) Ryujinx Team and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan_core.h>

#include "yuzu_shader_recompiler/profile.h"

namespace Vulkan::MoltenVK {

// MoltenVK must not enable unsupported depth-bounds dynamic states.
constexpr bool UseDepthBoundsDynamicState(VkDriverId driver, bool supported) {
    return driver != VK_DRIVER_ID_MOLTENVK || supported;
}
// Larger MoltenVK push layouts can produce unstable
// Metal bindings. Use ordinary descriptor sets beyond this conservative limit.
constexpr uint32_t PushDescriptorLimit(VkDriverId driver, uint32_t advertised_limit) {
    return driver == VK_DRIVER_ID_MOLTENVK ? std::min(advertised_limit, 16U) : advertised_limit;
}

struct TexelBufferRange {
    uint32_t offset;
    uint32_t size;
    // Elements at the end of the requested range that the view cannot hold
    uint32_t lost_elements;
    // False when the view cannot start at the requested offset: it is created there and the host
    // reads from the offset rounded down, as it does without this workaround
    bool aligned;
};

// Metal rounds the offset of a buffer-backed texture down to its alignment instead of rejecting
// it. A texel buffer view that must start elsewhere is created at the aligned offset instead, and
// the elements between the two are counted in the low Shader::TEXEL_BUFFER_PAD_BITS bits of its
// element count, where shaders read them (Shader::Profile::texel_buffer_pad_in_size).
//
// Shaders decode every view this way, so the element count is always rounded up to keep those
// bits equal to the padding, which is zero when the view is not shifted. buffer_size is the size of
// the host buffer including the Shader::TEXEL_BUFFER_TAIL_BYTES past its data, which is what makes
// the rounding possible for any view inside the data. Without that room the view cannot be made
// decodable: it is returned as requested, or cut short with the missing elements counted in
// lost_elements.
constexpr TexelBufferRange AlignedTexelBufferRange(uint32_t alignment, uint64_t buffer_size,
                                                   uint32_t offset, uint32_t size,
                                                   uint32_t texel_size) {
    constexpr uint32_t GRANULE = 1U << Shader::TEXEL_BUFFER_PAD_BITS;
    const TexelBufferRange unchanged{offset, size, 0, false};
    if (size == 0 || texel_size == 0) {
        return unchanged;
    }
    const uint32_t pad_bytes = alignment > 1 ? offset % alignment : 0;
    // A shift that is not a whole number of elements cannot be skipped by the shader
    const bool shiftable = pad_bytes % texel_size == 0;
    const uint64_t start = shiftable ? offset - pad_bytes : offset;
    const uint64_t pad = shiftable ? pad_bytes / texel_size : 0;
    if (start >= buffer_size) {
        return unchanged;
    }
    const uint64_t buffer_elements = (buffer_size - start) / texel_size;
    if (buffer_elements <= pad) {
        return unchanged;
    }
    const uint64_t needed = size / texel_size;
    const uint64_t wanted = (needed + GRANULE - 1) / GRANULE * GRANULE;
    const uint64_t fitting = (buffer_elements - pad) / GRANULE * GRANULE;
    const uint64_t elements = std::min(wanted, fitting);
    if (elements == 0) {
        return unchanged;
    }
    return {static_cast<uint32_t>(start), static_cast<uint32_t>((pad + elements) * texel_size),
            static_cast<uint32_t>(needed > elements ? needed - elements : 0), shiftable};
}

constexpr bool SubresourceRangesOverlap(const VkImageSubresourceRange& lhs,
                                        const VkImageSubresourceRange& rhs) {
    if ((lhs.aspectMask & rhs.aspectMask) == 0 || lhs.levelCount == 0 || rhs.levelCount == 0 ||
        lhs.layerCount == 0 || rhs.layerCount == 0) {
        return false;
    }
    const uint64_t lhs_mip_end = static_cast<uint64_t>(lhs.baseMipLevel) + lhs.levelCount;
    const uint64_t rhs_mip_end = static_cast<uint64_t>(rhs.baseMipLevel) + rhs.levelCount;
    const uint64_t lhs_layer_end = static_cast<uint64_t>(lhs.baseArrayLayer) + lhs.layerCount;
    const uint64_t rhs_layer_end = static_cast<uint64_t>(rhs.baseArrayLayer) + rhs.layerCount;
    return lhs.baseMipLevel < rhs_mip_end && rhs.baseMipLevel < lhs_mip_end &&
           lhs.baseArrayLayer < rhs_layer_end && rhs.baseArrayLayer < lhs_layer_end;
}

template <typename Handle>
constexpr bool ColorAttachmentsAlias(Handle lhs_image, const VkImageSubresourceRange& lhs_range,
                                     Handle rhs_image, const VkImageSubresourceRange& rhs_range) {
    return lhs_image != Handle{} && lhs_image == rhs_image &&
           SubresourceRangesOverlap(lhs_range, rhs_range);
}

// Compare underlying images, including different views.
// Keep disjoint mip/layer views and record actual overlapping pairs, not transitive groups.
template <typename Handle, size_t N>
constexpr std::array<uint32_t, N> ColorAttachmentAliasMasks(
    VkDriverId driver, const std::array<Handle, N>& images,
    const std::array<VkImageSubresourceRange, N>& ranges) {
    static_assert(N <= 32);
    std::array<uint32_t, N> aliases{};
    if (driver != VK_DRIVER_ID_MOLTENVK) {
        return aliases;
    }
    for (size_t slot = 0; slot < N; ++slot) {
        for (size_t previous = 0; previous < slot; ++previous) {
            if (ColorAttachmentsAlias(images[slot], ranges[slot], images[previous], ranges[previous])) {
                aliases[slot] |= 1U << previous;
                aliases[previous] |= 1U << slot;
            }
        }
    }
    return aliases;
}

} // namespace Vulkan::MoltenVK
