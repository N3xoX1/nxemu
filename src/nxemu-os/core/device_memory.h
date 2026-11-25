// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "yuzu_common/host_memory.h"
#include "yuzu_common/typed_address.h"
#include <nxemu-module-spec/operating_system.h>

namespace Core {

namespace DramMemoryMap {
enum : u64 {
    Base = 0x80000000ULL,
    KernelReserveBase = Base + 0x60000,
    SlabHeapBase = KernelReserveBase + 0x85000,
};
}; // namespace DramMemoryMap

class DeviceMemory :
    public IDeviceMemory
{
public:
    explicit DeviceMemory();
    ~DeviceMemory();

    DeviceMemory& operator=(const DeviceMemory&) = delete;
    DeviceMemory(const DeviceMemory&) = delete;

    //IDeviceMemory
    const uint8_t * BackingBasePointer() const
    {
        return buffer.BackingBasePointer();
    }

    uint64_t BackingSize() const
    {
        return buffer.BackingSize();
    }

    uint8_t * VirtualBasePointer()
    {
        return buffer.VirtualBasePointer();
    }

    void EnableDirectMappedAddress()
    {
        buffer.EnableDirectMappedAddress();
    }

    void MarkBackingPinned(uint64_t physical_offset, uint64_t length) const
    {
        buffer.MarkBackingPinned(static_cast<size_t>(physical_offset), static_cast<size_t>(length));
    }

    template <typename T>
    Common::PhysicalAddress GetPhysicalAddr(const T* ptr) const {
        return (reinterpret_cast<uintptr_t>(ptr) -
                reinterpret_cast<uintptr_t>(buffer.BackingBasePointer())) +
               DramMemoryMap::Base;
    }

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) -
                                  reinterpret_cast<uintptr_t>(buffer.BackingBasePointer()));
    }

    template <typename T>
    T* GetPointer(Common::PhysicalAddress addr) {
        return reinterpret_cast<T*>(buffer.BackingBasePointer() +
                                    (addr.GetValue() - DramMemoryMap::Base));
    }

    template <typename T>
    const T* GetPointer(Common::PhysicalAddress addr) const {
        return reinterpret_cast<T*>(buffer.BackingBasePointer() +
                                    (addr.GetValue() - DramMemoryMap::Base));
    }

    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(buffer.BackingBasePointer() + addr);
    }

    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<T*>(buffer.BackingBasePointer() + addr);
    }

    Common::HostMemory buffer;

private:
    void NotifyTrimActivity();
    void MaintainGuestMemory(std::stop_token stop);

    std::mutex trim_wake_mutex;
    std::condition_variable_any trim_wake_cv;
    std::atomic<bool> trim_requested{false};
    // Destroyed before buffer, stopping all reclamation before its mappings go away.
    std::jthread memory_maintenance_thread;
};

} // namespace Core
