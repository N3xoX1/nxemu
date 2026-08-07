// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::BCAT {
class BcatBackend;
class IBcatService;
class IDeliveryCacheStorageService;

class IServiceCreator final : public ServiceFramework<IServiceCreator> {
public:
    explicit IServiceCreator(Core::System& system_, const char* name_);
    ~IServiceCreator() override;

private:
    Result CreateBcatService(ClientProcessId process_id, OutInterface<IBcatService> out_interface);
    Result CreateDeliveryCacheStorageService(ClientProcessId process_id, OutInterface<IDeliveryCacheStorageService> out_interface);
    Result CreateDeliveryCacheStorageServiceWithApplicationId(u64 application_id, OutInterface<IDeliveryCacheStorageService> out_interface);

    std::unique_ptr<BcatBackend> backend;
};

} // namespace Service::BCAT
