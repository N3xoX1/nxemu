// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/metal_common/metal_memory_allocator.h"

namespace Metal {
namespace {
// Apple Silicon (unified memory) never allocates Managed buffers; skipping the
// Objective-C storageMode() query on FlushRange saves 600k+ calls/sec.
std::atomic<bool> g_has_managed_buffers{false};
} // namespace

MemoryAllocator::MemoryAllocator(const Device& device_) : device{device_} {
    unified_memory = device.HasUnifiedMemory();
    // Memoryless only elides the allocation on tile-based hardware.
    memoryless_attachments = device.IsTileBased();

    LOG_INFO(Render_Metal, "Memory: unified={} direct_upload={} memoryless_attachments={}",
             unified_memory, PrefersDirectUpload(), memoryless_attachments);
}

MemoryAllocator::~MemoryAllocator() = default;

MTL::StorageMode MemoryAllocator::StorageModeFor(MemoryUsage usage) const noexcept {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return unified_memory ? MTL::StorageModeShared : MTL::StorageModePrivate;
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        if (unified_memory) {
            return MTL::StorageModeShared;
        }
        // Discrete: Managed keeps a CPU copy mirrored on didModifyRange.
        return MTL::StorageModeManaged;
    case MemoryUsage::Download:
        return MTL::StorageModeShared;
    case MemoryUsage::Transient:
        return memoryless_attachments ? MTL::StorageModeMemoryless : MTL::StorageModePrivate;
    }
    return MTL::StorageModeShared;
}

MTL::ResourceOptions MemoryAllocator::ResourceOptionsFor(MemoryUsage usage) const noexcept {
    MTL::ResourceOptions options{};

    switch (StorageModeFor(usage)) {
    case MTL::StorageModeShared:
        options |= MTL::ResourceStorageModeShared;
        break;
    case MTL::StorageModeManaged:
        options |= MTL::ResourceStorageModeManaged;
        break;
    case MTL::StorageModePrivate:
        options |= MTL::ResourceStorageModePrivate;
        break;
    case MTL::StorageModeMemoryless:
        options |= MTL::ResourceStorageModeMemoryless;
        break;
    }

    // Write-combined skips the CPU read cache. Never apply it to Download:
    // reading write-combined memory is pathologically slow.
    if (usage == MemoryUsage::Upload || usage == MemoryUsage::Stream) {
        options |= MTL::ResourceCPUCacheModeWriteCombined;
    } else {
        options |= MTL::ResourceCPUCacheModeDefaultCache;
    }

    // No hazard tracking mode: Metal 4 considers every resource untracked and
    // ignores the property. Ordering comes from explicit barriers in Scheduler.

    return options;
}

Owned<MTL::Buffer> MemoryAllocator::CreateBuffer(u64 size, MemoryUsage usage) const {
    if (size == 0) {
        return {};
    }

    if (size > device.GetMaxBufferLength()) {
        LOG_ERROR(Render_Metal, "Buffer of {} bytes exceeds the device maximum of {}", size,
                  device.GetMaxBufferLength());
        return {};
    }

    // No padding here, on purpose. Metal 4 binds a buffer as a bare GPU address
    // with no length and no bounds checking, but padding every allocation with
    // 64 KiB did not improve the fault rate at all.
    const MTL::ResourceOptions options = ResourceOptionsFor(usage);
    if (StorageModeFor(usage) == MTL::StorageModeManaged) {
        g_has_managed_buffers.store(true, std::memory_order_relaxed);
    }
    Owned<MTL::Buffer> buffer = Adopt(device.GetDevice()->newBuffer(size, options));
    if (!buffer) {
        LOG_ERROR(Render_Metal, "Failed to allocate a {} byte buffer", size);
        return buffer;
    }
    device.AddResidentAllocation(buffer.get());
    return buffer;
}

Owned<MTL::Texture> MemoryAllocator::CreateTexture(MTL::TextureDescriptor* descriptor,
                                                    MemoryUsage usage) const {
    if (descriptor == nullptr) {
        return {};
    }

    // Device-local *textures* are Private even on unified memory: Shared disables
    // AGX lossless compression, which costs bandwidth on every attachment
    // store/load and texture fetch. Uploads arrive by blit, so nothing is lost.
    const MTL::StorageMode storage_mode = usage == MemoryUsage::DeviceLocal
                                              ? MTL::StorageModePrivate
                                              : StorageModeFor(usage);
    descriptor->setStorageMode(storage_mode);

    if (storage_mode == MTL::StorageModeMemoryless) {
        // Shader or blit access to tile memory is a validation error.
        descriptor->setUsage(MTL::TextureUsageRenderTarget);
    }

    Owned<MTL::Texture> texture = Adopt(device.GetDevice()->newTexture(descriptor));
    if (!texture) {
        LOG_ERROR(Render_Metal, "Failed to allocate a {}x{} texture", descriptor->width(),
                  descriptor->height());
        return texture;
    }
    device.AddResidentAllocation(texture.get());
    return texture;
}

std::span<u8> MemoryAllocator::Map(MTL::Buffer* buffer) {
    if (buffer == nullptr) {
        return {};
    }
    // contents() returns null for Private and memoryless buffers, it does not fail.
    void* contents = buffer->contents();
    if (contents == nullptr) {
        return {};
    }
    return std::span<u8>{static_cast<u8*>(contents), static_cast<size_t>(buffer->length())};
}

bool MemoryAllocator::HasManagedBuffers() noexcept {
    return g_has_managed_buffers.load(std::memory_order_relaxed);
}

void MemoryAllocator::FlushRange(MTL::Buffer* buffer, u64 offset, u64 size) {
    if (!g_has_managed_buffers.load(std::memory_order_relaxed) || buffer == nullptr || size == 0) {
        return;
    }
    // On Shared the write is already visible and didModifyRange is a validation error.
    if (buffer->storageMode() != MTL::StorageModeManaged) {
        return;
    }
    buffer->didModifyRange(NS::Range::Make(offset, size));
}

} // namespace Metal
