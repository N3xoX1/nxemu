// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Architecture based in part on Ryubing/Ryujinx AppleHv (MIT):
// Copyright (c) Ryujinx Team and Contributors.

#pragma once

#include <memory>
#include <shared_mutex>

#include "yuzu_common/common_types.h"
#include "yuzu_common/page_table.h"

class IDeviceMemory;
class IKProcessPageTable;

namespace Core::AppleHv {

/// Stage-1 AArch64 address space used by the Apple Hypervisor NCE backend.
///
/// Guest virtual addresses are translated by real ARM page tables to an IPA range. Hypervisor.framework
/// then translates that IPA range to NXEmu's ordinary DeviceMemory backing allocation. This deliberately
/// avoids the in-process guest-VA == host-VA requirement of the legacy NCE fastmem design.
class AddressSpace final : public Common::NativeMemoryObserver {
public:
    AddressSpace(IDeviceMemory& device_memory, IKProcessPageTable& page_table, u32 address_space_bits = 39);
    ~AddressSpace() override;

    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    void DetachPageTable();
    static std::shared_ptr<void> RetainVm();
    [[nodiscard]] u64 Identity() const noexcept;
    [[nodiscard]] u64 CodeGeneration() const noexcept;
    [[nodiscard]] u64 KernelTlbOnlyEretAddress() const noexcept;
    [[nodiscard]] u64 Generation() const noexcept;
    [[nodiscard]] u64 KernelEretAddress() const noexcept;
    [[nodiscard]] u64 KernelTlbiEretAddress() const noexcept;

    /// Installs TTBR0/TTBR1 and the EL1 exception-vector configuration on a newly-created vCPU.
    bool InitializeVcpu(u64 vcpu) const;

    /// Active vCPUs are registered so page-table changes can kick them out of hv_vcpu_run().
    std::shared_lock<std::shared_mutex> LockExecution(std::atomic_bool& running);
    void InvalidateCode();
    u64 MappingGranule(u64 va);
    void LogFault(u64 va);
    bool AllowsAccess(u64 address, Common::MemoryPermission access);
    void RegisterVcpu(u64 vcpu, std::atomic_bool& running);
    void UnregisterVcpu(u64 vcpu);

    void Map(Common::ProcessAddress virtual_address, Common::PhysicalAddress physical_address,
             u64 size, Common::MemoryPermission permissions) override;
    void Track(Common::ProcessAddress virtual_address, u64 size,
               Common::MemoryPermission permissions) override;
    void TrackBatch(std::span<const Common::NativeMemoryTrackRange> ranges) override;
    void Unmap(Common::ProcessAddress virtual_address, u64 size) override;
    void Protect(Common::ProcessAddress virtual_address, u64 size,
                 Common::MemoryPermission permissions) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    IKProcessPageTable* m_page_table{};
};

} // namespace Core::AppleHv
