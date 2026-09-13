// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/alignment.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_buffer.h"
#include "core/hle/kernel/memory_types.h"

namespace Kernel {

KPageBuffer* KPageBuffer::FromPhysicalAddress(Core::System& system, KPhysicalAddress phys_addr) {
    ASSERT(Common::IsAligned(phys_addr.GetValue(), PageSize));
    return system.DeviceMemory().GetPointer<KPageBuffer>(phys_addr);
}

} // namespace Kernel
