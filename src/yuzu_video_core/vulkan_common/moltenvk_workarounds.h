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
    // The view shaders read: it starts at an offset the host accepts, possibly before the range
    // the guest bound
    uint32_t offset;
    uint32_t size;
    // Size of the second view, which starts at the same offset: its element count is the size the
    // guest bound
    uint32_t size_view_size;
    // False when the range starts at an offset the host rounds down and that cannot be skipped
    bool aligned;
};

// Metal rounds the offset of a buffer-backed texture down to its alignment instead of rejecting
// it. A texel buffer view that must start elsewhere is created at the aligned offset instead, and
// the shader skips the elements before the range the guest bound
// (Shader::Profile::texel_buffer_size_view). The element count of the second view is the size the
// guest bound, and the difference with the element count of the first one is how many elements
// to skip.
constexpr TexelBufferRange AlignedTexelBufferRange(uint32_t alignment, uint32_t offset,
                                                   uint32_t size, uint32_t texel_size) {
    const uint32_t pad_bytes = alignment > 1 ? offset % alignment : 0;
    if (pad_bytes == 0) {
        return {offset, size, size, true};
    }
    // A shift that is not a whole number of elements cannot be skipped by the shader
    if (texel_size == 0 || pad_bytes % texel_size != 0 || size > UINT32_MAX - pad_bytes) {
        return {offset, size, size, false};
    }
    return {offset - pad_bytes, size + pad_bytes, size, true};
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
