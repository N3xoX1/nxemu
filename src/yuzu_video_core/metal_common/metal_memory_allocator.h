// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

class Device;

/// How a resource will be used. Maps onto a Metal storage mode and a CPU cache
/// mode.
enum class MemoryUsage {
    DeviceLocal, ///< GPU-resident working set: render targets, cached images.
    Upload,      ///< CPU writes, GPU reads. Guest data on its way in.
    Download,    ///< GPU writes, CPU reads. Readbacks and queries.
    Stream,      ///< Short-lived per-frame CPU writes; uniforms and vertices.
    Transient,   ///< Never leaves tile memory. Attachments that are neither
                 ///< loaded nor stored. Only valid for render targets.
};

/// Allocates Metal buffers and textures with the right storage class. Not a
/// suballocator: Metal already suballocates inside an MTLHeap. What this
/// encodes is unified memory (no staging copy) and memoryless attachments.
class MemoryAllocator : Common::NonCopyable, Common::NonMoveable {
public:
    explicit MemoryAllocator(const Device& device);
    ~MemoryAllocator();

    /// Allocates a buffer. Returns null on failure rather than throwing; the
    /// caches treat allocation failure as a signal to evict and retry.
    [[nodiscard]] Owned<MTL::Buffer> CreateBuffer(u64 size, MemoryUsage usage) const;

    /// Allocates a texture from @p descriptor, overriding its storage mode to
    /// match @p usage.
    [[nodiscard]] Owned<MTL::Texture> CreateTexture(MTL::TextureDescriptor* descriptor,
                                                     MemoryUsage usage) const;

    [[nodiscard]] MTL::StorageMode StorageModeFor(MemoryUsage usage) const noexcept;
    [[nodiscard]] MTL::ResourceOptions ResourceOptionsFor(MemoryUsage usage) const noexcept;

    /// True when Upload buffers are directly GPU-visible, so guest data can be
    /// written straight into the destination with no staging copy. Ask rather
    /// than assume, a discrete GPU still needs real staging.
    [[nodiscard]] bool PrefersDirectUpload() const noexcept {
        return unified_memory;
    }

    /// Whether MemoryUsage::Transient actually elides the allocation. False on
    /// immediate-mode GPUs, where it silently degrades to a private texture.
    [[nodiscard]] bool SupportsMemorylessAttachments() const noexcept {
        return memoryless_attachments;
    }

    /// Maps a host-visible buffer. Returns an empty span for Private storage.
    [[nodiscard]] static std::span<u8> Map(MTL::Buffer* buffer);

    /// Flushes a CPU write on storage modes that need it. A no-op on Shared
    /// memory, which covers every Apple Silicon and iOS device, so callers can
    /// invoke it unconditionally.
    static void FlushRange(MTL::Buffer* buffer, u64 offset, u64 size);

    /// True if any StorageModeManaged buffer has been allocated in this process.
    /// Allows callers on unified memory (Apple Silicon) to skip ObjC queries like
    /// buffer->length() that only exist to feed FlushRange.
    [[nodiscard]] static bool HasManagedBuffers() noexcept;

private:
    const Device& device;

    bool unified_memory{};
    bool memoryless_attachments{};
};

} // namespace Metal
