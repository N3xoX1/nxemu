// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <utility>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/cmif_serialization.h"
#include "nxemu-module-spec/system_loader.h"

namespace Service::FileSystem
{

template <typename ReadHandler>
void ReplyWithStorageReadTiming(Core::System & system, HLERequestContext & ctx, ReadHandler && read)
{
    auto & loader = system.GetSystemloader();
    StorageReadTraffic before{};
    loader.GetBackingReadTraffic(&before);
    const auto start = static_cast<u64>(system.CoreTiming().GetGlobalTimeNs().count());
    std::forward<ReadHandler>(read)();
    StorageReadTraffic after{};
    loader.GetBackingReadTraffic(&after);
    for (size_t domain = 0; domain < static_cast<size_t>(StorageReadDomain::Count); ++domain)
    {
        const auto bytes = after.bytes[domain] - before.bytes[domain];
        if (bytes)
        {
            const auto deadline = system.CoreTiming().ReserveStorageRead(static_cast<StorageReadDomain>(domain), start, bytes);
            ctx.SetCompletionTime(std::max(ctx.GetCompletionTime(), deadline));
        }
    }
}

} // namespace Service::FileSystem
