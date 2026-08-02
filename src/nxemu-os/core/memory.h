// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nxemu-module-spec/cpu.h>

#include "yuzu_common/guest_memory.h"
#include "yuzu_common/typed_address.h"
#include "core/hle/result.h"

namespace Common {
enum class MemoryPermission : u32;
struct PageTable;
} // namespace Common

namespace Core {
class System;
class GPUDirtyMemoryManager;
} // namespace Core

namespace Kernel {
class KProcess;
} // namespace Kernel

namespace Tegra {
class MemoryManager;
}

namespace Core::Memory {

/**
 * Page size used by the ARM architecture. This is the smallest granularity with which memory can
 * be mapped.
 */
constexpr std::size_t YUZU_PAGEBITS = 12;
constexpr u64 YUZU_PAGESIZE = 1ULL << YUZU_PAGEBITS;
constexpr u64 YUZU_PAGEMASK = YUZU_PAGESIZE - 1;

/// Virtual user-space memory regions
enum : u64 {
    /// TLS (Thread-Local Storage) related.
    TLS_ENTRY_SIZE = 0x200,

    /// Application stack
    DEFAULT_STACK_SIZE = 0x100000,
};

/// Central class that handles all memory operations and state.
class Memory :
    public IMemory
{
public:
    explicit Memory(Core::System& system);
    ~Memory();

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    Memory(Memory&&) = default;
    Memory& operator=(Memory&&) = delete;

    /**
     * Resets the state of the Memory system.
     */
    void Reset();

    /**
     * Changes the currently active page table to that of the given process instance.
     *
     * @param process The process to use the page table of.
     */
    void SetCurrentPageTable(Kernel::KProcess& process);

    /**
     * Maps an allocated buffer onto a region of the emulated process address space.
     *
     * @param page_table The page table of the emulated process.
     * @param base       The address to start mapping at. Must be page-aligned.
     * @param size       The amount of bytes to map. Must be page-aligned.
     * @param target     Buffer with the memory backing the mapping. Must be of length at least
     *                   `size`.
     * @param perms      The permissions to map the memory with.
     */
    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap);

    /**
     * Unmaps a region of the emulated process address space.
     *
     * @param page_table The page table of the emulated process.
     * @param base       The address to begin unmapping at.
     * @param size       The amount of bytes to unmap.
     */
    void UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                     bool separate_heap);

    /**
     * Protects a region of the emulated process address space with the new permissions.
     *
     * @param page_table The page table of the emulated process.
     * @param base       The start address to re-protect. Must be page-aligned.
     * @param size       The amount of bytes to protect. Must be page-aligned.
     * @param perms      The permissions the address range is mapped.
     */
    void ProtectRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                       Common::MemoryPermission perms);

    /**
     * Checks whether or not the supplied address is a valid virtual
     * address for the current process.
     *
     * @param vaddr The virtual address to check the validity of.
     *
     * @returns True if the given virtual address is valid, false otherwise.
     */
    [[nodiscard]] bool IsValidVirtualAddress(Common::ProcessAddress vaddr) const;

    /**
     * Checks whether or not the supplied range of addresses are all valid
     * virtual addresses for the current process.
     *
     * @param base The address to begin checking.
     * @param size The amount of bytes to check.
     *
     * @returns True if all bytes in the given range are valid, false otherwise.
     */
    [[nodiscard]] bool IsValidVirtualAddressRange(uint64_t base, uint64_t size) const override;

    /**
     * Gets a pointer to the given address.
     *
     * @param vaddr Virtual address to retrieve a pointer to.
     *
     * @returns The pointer to the given address, if the address is valid.
     *          If the address is not valid, nullptr will be returned.
     */
    u8* GetPointer(Common::ProcessAddress vaddr);
    u8* GetPointerSilent(uint64_t vaddr) override;

    template <typename T>
    T* GetPointer(Common::ProcessAddress vaddr) {
        return reinterpret_cast<T*>(GetPointer(vaddr));
    }

    /**
     * Gets a pointer to the given address.
     *
     * @param vaddr Virtual address to retrieve a pointer to.
     *
     * @returns The pointer to the given address, if the address is valid.
     *          If the address is not valid, nullptr will be returned.
     */
    [[nodiscard]] const u8* GetPointer(Common::ProcessAddress vaddr) const;

    template <typename T>
    const T* GetPointer(Common::ProcessAddress vaddr) const {
        return reinterpret_cast<T*>(GetPointer(vaddr));
    }

    /**
     * Reads an 8-bit unsigned value from the current process' address space
     * at the given virtual address.
     *
     * @param addr The virtual address to read the 8-bit value from.
     *
     * @returns the read 8-bit unsigned value.
     */
    uint8_t Read8(uint64_t addr) override;

    /**
     * Reads a 16-bit unsigned value from the current process' address space
     * at the given virtual address.
     *
     * @param addr The virtual address to read the 16-bit value from.
     *
     * @returns the read 16-bit unsigned value.
     */
    uint16_t Read16(uint64_t addr) override;

    /**
     * Reads a 32-bit unsigned value from the current process' address space
     * at the given virtual address.
     *
     * @param addr The virtual address to read the 32-bit value from.
     *
     * @returns the read 32-bit unsigned value.
     */
    uint32_t Read32(uint64_t addr) override;

    /**
     * Reads a 64-bit unsigned value from the current process' address space
     * at the given virtual address.
     *
     * @param addr The virtual address to read the 64-bit value from.
     *
     * @returns the read 64-bit value.
     */
    uint64_t Read64(uint64_t addr) override;

    /**
     * Writes an 8-bit unsigned integer to the given virtual address in
     * the current process' address space.
     *
     * @param addr The virtual address to write the 8-bit unsigned integer to.
     * @param data The 8-bit unsigned integer to write to the given virtual address.
     *
     * @post The memory at the given virtual address contains the specified data value.
     */
    void Write8(uint64_t addr, uint8_t data) override;

    /**
     * Writes a 16-bit unsigned integer to the given virtual address in
     * the current process' address space.
     *
     * @param addr The virtual address to write the 16-bit unsigned integer to.
     * @param data The 16-bit unsigned integer to write to the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    void Write16(uint64_t addr, uint16_t data) override;

    /**
     * Writes a 32-bit unsigned integer to the given virtual address in
     * the current process' address space.
     *
     * @param addr The virtual address to write the 32-bit unsigned integer to.
     * @param data The 32-bit unsigned integer to write to the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    void Write32(uint64_t addr, uint32_t data) override;

    /**
     * Writes a 64-bit unsigned integer to the given virtual address in
     * the current process' address space.
     *
     * @param addr The virtual address to write the 64-bit unsigned integer to.
     * @param data The 64-bit unsigned integer to write to the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    void Write64(uint64_t addr, uint64_t data) override;

    /**
     * Writes a 8-bit unsigned integer to the given virtual address in
     * the current process' address space if and only if the address contains
     * the expected value. This operation is atomic.
     *
     * @param addr The virtual address to write the 8-bit unsigned integer to.
     * @param data The 8-bit unsigned integer to write to the given virtual address.
     * @param expected The 8-bit unsigned integer to check against the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    bool WriteExclusive8(uint64_t addr, uint8_t data, uint8_t expected) override;

    /**
     * Writes a 16-bit unsigned integer to the given virtual address in
     * the current process' address space if and only if the address contains
     * the expected value. This operation is atomic.
     *
     * @param addr The virtual address to write the 16-bit unsigned integer to.
     * @param data The 16-bit unsigned integer to write to the given virtual address.
     * @param expected The 16-bit unsigned integer to check against the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    bool WriteExclusive16(uint64_t addr, uint16_t data, uint16_t expected) override;

    /**
     * Writes a 32-bit unsigned integer to the given virtual address in
     * the current process' address space if and only if the address contains
     * the expected value. This operation is atomic.
     *
     * @param addr The virtual address to write the 32-bit unsigned integer to.
     * @param data The 32-bit unsigned integer to write to the given virtual address.
     * @param expected The 32-bit unsigned integer to check against the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    bool WriteExclusive32(uint64_t addr, uint32_t data, uint32_t expected) override;

    /**
     * Writes a 64-bit unsigned integer to the given virtual address in
     * the current process' address space if and only if the address contains
     * the expected value. This operation is atomic.
     *
     * @param addr The virtual address to write the 64-bit unsigned integer to.
     * @param data The 64-bit unsigned integer to write to the given virtual address.
     * @param expected The 64-bit unsigned integer to check against the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    bool WriteExclusive64(uint64_t addr, uint64_t data, uint64_t expected) override;

    /**
     * Writes a 128-bit unsigned integer to the given virtual address in
     * the current process' address space if and only if the address contains
     * the expected value. This operation is atomic.
     *
     * @param addr The virtual address to write the 128-bit unsigned integer to.
     * @param data The 128-bit unsigned integer to write to the given virtual address.
     * @param expected The 128-bit unsigned integer to check against the given virtual address.
     *
     * @post The memory range [addr, sizeof(data)) contains the given data value.
     */
    bool WriteExclusive128(uint64_t addr, uint64_t dataHi, uint64_t dataLow, uint64_t expectedHi, uint64_t expectedLow) override;

    /**
     * Reads a null-terminated string from the given virtual address.
     * This function will continually read characters until either:
     *
     * - A null character ('\0') is reached.
     * - max_length characters have been read.
     *
     * @note The final null-terminating character (if found) is not included
     *       in the returned string.
     *
     * @param vaddr      The address to begin reading the string from.
     * @param max_length The maximum length of the string to read in characters.
     *
     * @returns The read string.
     */
    std::string ReadCString(Common::ProcessAddress vaddr, std::size_t max_length);

    /**
     * Reads a contiguous block of bytes from the current process' address space.
     *
     * @param src_addr    The virtual address to begin reading from.
     * @param dest_buffer The buffer to place the read bytes into.
     * @param size        The amount of data to read, in bytes.
     *
     * @note If a size of 0 is specified, then this function reads nothing and
     *       no attempts to access memory are made at all.
     *
     * @pre dest_buffer must be at least size bytes in length, otherwise a
     *      buffer overrun will occur.
     *
     * @post The range [dest_buffer, size) contains the read bytes from the
     *       current process' address space.
     */
    bool ReadBlock(uint64_t src_addr, void * dest_buffer, uint64_t size) override;

    /**
     * Reads a contiguous block of bytes from the current process' address space.
     * This unsafe version does not trigger GPU flushing.
     *
     * @param src_addr    The virtual address to begin reading from.
     * @param dest_buffer The buffer to place the read bytes into.
     * @param size        The amount of data to read, in bytes.
     *
     * @note If a size of 0 is specified, then this function reads nothing and
     *       no attempts to access memory are made at all.
     *
     * @pre dest_buffer must be at least size bytes in length, otherwise a
     *      buffer overrun will occur.
     *
     * @post The range [dest_buffer, size) contains the read bytes from the
     *       current process' address space.
     */
    bool ReadBlockUnsafe(Common::ProcessAddress src_addr, void* dest_buffer, std::size_t size);

    const u8* GetSpan(const VAddr src_addr, const std::size_t size) const;
    u8* GetSpan(const VAddr src_addr, const std::size_t size);

    /**
     * Writes a range of bytes into the current process' address space at the specified
     * virtual address.
     *
     * @param dest_addr  The destination virtual address to begin writing the data at.
     * @param src_buffer The data to write into the current process' address space.
     * @param size       The size of the data to write, in bytes.
     *
     * @post The address range [dest_addr, size) in the current process' address space
     *       contains the data that was within src_buffer.
     *
     * @post If an attempt is made to write into an unmapped region of memory, the writes
     *       will be ignored and an error will be logged.
     *
     * @post If a write is performed into a region of memory that is considered cached
     *       rasterizer memory, will cause the currently active rasterizer to be notified
     *       and will mark that region as invalidated to caches that the active
     *       graphics backend may be maintaining over the course of execution.
     */
    bool WriteBlock(Common::ProcessAddress dest_addr, const void* src_buffer, std::size_t size);

    /**
     * Writes a range of bytes into the current process' address space at the specified
     * virtual address.
     * This unsafe version does not invalidate GPU Memory.
     *
     * @param dest_addr  The destination virtual address to begin writing the data at.
     * @param src_buffer The data to write into the current process' address space.
     * @param size       The size of the data to write, in bytes.
     *
     * @post The address range [dest_addr, size) in the current process' address space
     *       contains the data that was within src_buffer.
     *
     * @post If an attempt is made to write into an unmapped region of memory, the writes
     *       will be ignored and an error will be logged.
     *
     */
    bool WriteBlockUnsafe(Common::ProcessAddress dest_addr, const void* src_buffer,
                          std::size_t size);

    /**
     * Copies data within a process' address space to another location within the
     * same address space.
     *
     * @param dest_addr The destination virtual address to begin copying the data into.
     * @param src_addr  The source virtual address to begin copying the data from.
     * @param size      The size of the data to copy, in bytes.
     *
     * @post The range [dest_addr, size) within the process' address space contains the
     *       same data within the range [src_addr, size).
     */
    bool CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                   std::size_t size);

    /**
     * Zeros a range of bytes within the current process' address space at the specified
     * virtual address.
     *
     * @param dest_addr The destination virtual address to zero the data from.
     * @param size      The size of the range to zero out, in bytes.
     *
     * @post The range [dest_addr, size) within the process' address space contains the
     *       value 0.
     */
    bool ZeroBlock(Common::ProcessAddress dest_addr, std::size_t size);

    /**
     * Invalidates a range of bytes within the current process' address space at the specified
     * virtual address.
     *
     * @param dest_addr The destination virtual address to invalidate the data from.
     * @param size      The size of the range to invalidate, in bytes.
     *
     */
    Result InvalidateDataCache(Common::ProcessAddress dest_addr, std::size_t size);

    /**
     * Stores a range of bytes within the current process' address space at the specified
     * virtual address.
     *
     * @param dest_addr The destination virtual address to store the data from.
     * @param size      The size of the range to store, in bytes.
     *
     */
    Result StoreDataCache(Common::ProcessAddress dest_addr, std::size_t size);

    /**
     * Flushes a range of bytes within the current process' address space at the specified
     * virtual address.
     *
     * @param dest_addr The destination virtual address to flush the data from.
     * @param size      The size of the range to flush, in bytes.
     *
     */
    Result FlushDataCache(Common::ProcessAddress dest_addr, std::size_t size);

    /**
     * Marks each page within the specified address range as cached or uncached.
     *
     * @param vaddr  The virtual address indicating the start of the address range.
     * @param size   The size of the address range in bytes.
     * @param cached Whether or not any pages within the address range should be
     *               marked as cached or uncached.
     */
    void RasterizerMarkRegionCached(uint64_t vaddr, u64 size, bool cached) override;

    /**
     * Marks each page within the specified address range as debug or non-debug.
     * Debug addresses are not accessible from fastmem pointers.
     *
     * @param vaddr The virtual address indicating the start of the address range.
     * @param size  The size of the address range in bytes.
     * @param debug Whether or not any pages within the address range should be
     *              marked as debug or non-debug.
     */
    void MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug);

    void SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers);

    bool InvalidateNCE(Common::ProcessAddress vaddr, size_t size);

    bool InvalidateSeparateHeap(void* fault_address);

private:
    Core::System& system;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

template <typename T, GuestMemoryFlags FLAGS>
using CpuGuestMemory = GuestMemory<Memory, T, FLAGS>;
template <typename T, GuestMemoryFlags FLAGS>
using CpuGuestMemoryScoped = GuestMemoryScoped<Memory, T, FLAGS>;

} // namespace Core::Memory
