// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/virtual_buffer.h"

namespace Common {

enum class MemoryPermission : u32 {
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * A low level linear memory buffer, which supports multiple mappings
 * Its purpose is to rebuild a given sparse memory layout, including mirrors.
 */
class HostMemory {
public:
    explicit HostMemory(size_t backing_size_, size_t virtual_size_);
    ~HostMemory();

    /**
     * Copy constructors. They shall return a copy of the buffer without the mappings.
     * TODO: Implement them with COW if needed.
     */
    HostMemory(const HostMemory& other) = delete;
    HostMemory& operator=(const HostMemory& other) = delete;

    /**
     * Move constructors. They will move the buffer and the mappings to the new object.
     */
    HostMemory(HostMemory&& other) noexcept;
    HostMemory& operator=(HostMemory&& other) noexcept;

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms,
             bool separate_heap);

    void Unmap(size_t virtual_offset, size_t length, bool separate_heap);

    void Protect(size_t virtual_offset, size_t length, MemoryPermission perms);

    void EnableDirectMappedAddress();

    /// Limits host access to guest pages the GPU caches or the debugger watches, independently
    /// of their guest permission. Same as Protect when guest permissions are host permissions.
    void ProtectAccess(size_t virtual_offset, size_t length, MemoryPermission allowed);

    /// Whether guest memory is mapped for native code execution on 16 KiB pages.
    bool UsesNce() const;

    /// Native code execution on 16 KiB pages: whether the guest permission of the page holding
    /// `address` allows the access, so that the fault handler can emulate it.
    bool NceAccessAllowed(u64 address, bool write) const;

    /// Native code execution on 16 KiB pages: whether the host page holding `address` allows
    /// the access, or is about to.
    bool NceHostAccessAllowed(u64 address, bool write) const;

    /// Permission after GPU tracking of the individual 4 KiB guest page.
    bool NceTrackedAccessAllowed(u64 address, bool write) const;

    /// Native code execution on 16 KiB pages: host ranges holding guest memory.
    size_t GetNceWindows(u64* starts, u64* ends, size_t max) const;
    const void* GetNceAtomicTable() const;

    void ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value);

    [[nodiscard]] u8* BackingBasePointer() noexcept {
        return backing_base;
    }
    [[nodiscard]] const u8* BackingBasePointer() const noexcept {
        return backing_base;
    }
    [[nodiscard]] size_t BackingSize() const noexcept {
        return backing_size;
    }

    [[nodiscard]] u8* VirtualBasePointer() noexcept {
        return virtual_base;
    }
    [[nodiscard]] const u8* VirtualBasePointer() const noexcept {
        return virtual_base;
    }

    bool IsInVirtualRange(void* address) const noexcept {
        return virtual_base != nullptr && address >= virtual_base &&
               address < virtual_base + virtual_size;
    }

private:
    size_t backing_size{};
    size_t virtual_size{};

    // Low level handler for the platform dependent memory routines
    class Impl;
    std::unique_ptr<Impl> impl;
    u8* backing_base{};
    u8* virtual_base{};
    size_t virtual_base_offset{};

    // Fallback if fastmem is not supported on this platform
    std::unique_ptr<Common::VirtualBuffer<u8>> fallback_buffer;
};

} // namespace Common
