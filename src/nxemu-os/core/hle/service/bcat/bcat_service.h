// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/bcat/bcat_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::BCAT {
class BcatBackend;
class IDeliveryCacheStorageService;
class IDeliveryCacheProgressService;

class IBcatService final : public ServiceFramework<IBcatService> {
public:
    explicit IBcatService(Core::System& system_, BcatBackend& backend_);
    ~IBcatService() override;

private:
    Result RequestSyncDeliveryCache(OutInterface<IDeliveryCacheProgressService> out_interface);

    Result RequestSyncDeliveryCacheWithDirectoryName(
        const DirectoryName& name, OutInterface<IDeliveryCacheProgressService> out_interface);

    Result SetPassphrase(u64 application_id, InBuffer<BufferAttr_HipcPointer> passphrase_buffer);

    Result RegisterSystemApplicationDeliveryTasks();

    Result ClearDeliveryCacheStorage(u64 application_id);

private:
    ProgressServiceBackend& GetProgressBackend(SyncType type);
    const ProgressServiceBackend& GetProgressBackend(SyncType type) const;

    BcatBackend& backend;
    std::array<ProgressServiceBackend, static_cast<size_t>(SyncType::Count)> progress;
};

} // namespace Service::BCAT
