// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "yuzu_common/scope_exit.h"
#include "core/core.h"

#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_buffer.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread_local_page.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

Result KThreadLocalPage::Initialize(KernelCore& kernel, KProcess* process) {
    // Set that this process owns us.
    m_owner = process;
    m_kernel = std::addressof(kernel);

    // On hosts with pages larger than the guest's, give the page a whole host page of its own,
    // aligned like its address, so that it can be mapped directly.
    const size_t host_ratio = KMemoryManager::GuestPagesPerHostPage();
    if (host_ratio > 1) {
        const KPhysicalAddress block = kernel.MemoryManager().AllocateAndOpenContinuous(
            host_ratio, host_ratio,
            KMemoryManager::EncodeOption(KMemoryManager::Pool::System,
                                         KMemoryManager::Direction::FromBack));
        if (block != 0) {
            std::memset(kernel.System().DeviceMemory().GetPointer<void>(block), 0,
                        host_ratio * PageSize);
            const Result result = m_owner->GetKPageTable().MapPages(
                std::addressof(m_virt_addr), 1, host_ratio * PageSize, block,
                KMemoryState::ThreadLocal, KMemoryPermission::UserReadWrite);
            if (R_SUCCEEDED(result)) {
                m_host_block_pages = host_ratio;
                return ResultSuccess;
            }
            kernel.MemoryManager().Close(block, host_ratio);
        }
    }

    // Allocate a new page.
    KPageBuffer* page_buf = KPageBuffer::Allocate(kernel);
    R_UNLESS(page_buf != nullptr, ResultOutOfMemory);
    auto page_buf_guard = SCOPE_GUARD {
        KPageBuffer::Free(kernel, page_buf);
    };

    // Map the address in.
    const auto phys_addr = kernel.System().DeviceMemory().GetPhysicalAddr(page_buf);
    R_TRY(m_owner->GetKPageTable().MapPages(std::addressof(m_virt_addr), 1, PageSize, phys_addr,
                                           KMemoryState::ThreadLocal,
                                           KMemoryPermission::UserReadWrite));

    // We succeeded.
    page_buf_guard.Cancel();

    return ResultSuccess;
}

Result KThreadLocalPage::Finalize() {
    // Get the physical address of the page.
    KPhysicalAddress phys_addr{};
    ASSERT(m_owner->GetKPageTable().GetPhysicalAddress(std::addressof(phys_addr), m_virt_addr));

    // Unmap the page.
    R_TRY(m_owner->GetKPageTable().UnmapPages(this->GetAddress(), 1, KMemoryState::ThreadLocal));

    // Free the page.
    if (m_host_block_pages != 0) {
        m_kernel->MemoryManager().Close(phys_addr, m_host_block_pages);
    } else {
        KPageBuffer::Free(*m_kernel,
                          KPageBuffer::FromPhysicalAddress(m_kernel->System(), phys_addr));
    }

    return ResultSuccess;
}

KProcessAddress KThreadLocalPage::Reserve() {
    for (size_t i = 0; i < m_is_region_free.size(); i++) {
        if (m_is_region_free[i]) {
            m_is_region_free[i] = false;
            return this->GetRegionAddress(i);
        }
    }

    return 0;
}

void KThreadLocalPage::Release(KProcessAddress addr) {
    m_is_region_free[this->GetRegionIndex(addr)] = true;
}

} // namespace Kernel
