// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/bcat/bcat_service.h"
#include "core/hle/service/bcat/delivery_cache_storage_service.h"
#include "core/hle/service/bcat/service_creator.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IServiceCreator::IServiceCreator(Core::System& system_, const char* name_) :
    ServiceFramework{system_, name_}
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IServiceCreator::CreateBcatService>, "CreateBcatService"},
        {1, D<&IServiceCreator::CreateDeliveryCacheStorageService>, "CreateDeliveryCacheStorageService"},
        {2, D<&IServiceCreator::CreateDeliveryCacheStorageServiceWithApplicationId>, "CreateDeliveryCacheStorageServiceWithApplicationId"},
        {3, nullptr, "CreateDeliveryCacheProgressService"},
        {4, nullptr, "CreateDeliveryCacheProgressServiceWithApplicationId"},
    };
    // clang-format on

    RegisterHandlers(functions);

    backend = std::make_unique<BcatBackend>();
}

IServiceCreator::~IServiceCreator() = default;

Result IServiceCreator::CreateBcatService(ClientProcessId process_id, OutInterface<IBcatService> out_interface)
{
    LOG_INFO(Service_BCAT, "called, process_id={}", process_id.pid);
    *out_interface = std::make_shared<IBcatService>(system, *backend);
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheStorageService(ClientProcessId process_id, OutInterface<IDeliveryCacheStorageService> out_interface)
{
    LOG_INFO(Service_BCAT, "called, process_id={}", process_id.pid);

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheStorageServiceWithApplicationId(
    u64 application_id, OutInterface<IDeliveryCacheStorageService> out_interface) {
    LOG_DEBUG(Service_BCAT, "called, application_id={:016X}", application_id);
    UNIMPLEMENTED();
    R_SUCCEED();
}

} // namespace Service::BCAT
