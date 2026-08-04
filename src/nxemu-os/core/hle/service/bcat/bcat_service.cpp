// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/hex_util.h"
#include "yuzu_common/string_util.h"
#include "core/core.h"
#include "nxemu-loader/core/file_sys/errors.h"
#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_service.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_progress_service.h"
#include "core/hle/service/bcat/delivery_cache_storage_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

static u64 GetCurrentBuildID(const Core::System::CurrentBuildProcessID& id) {
    u64 out{};
    std::memcpy(&out, id.data(), sizeof(u64));
    return out;
}

IBcatService::IBcatService(Core::System& system_, BcatBackend& backend_)
    : ServiceFramework{system_, "IBcatService"}, backend{backend_},
      progress{{
          ProgressServiceBackend{system_, "Normal"},
          ProgressServiceBackend{system_, "Directory"},
      }} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {10100, D<&IBcatService::RequestSyncDeliveryCache>, "RequestSyncDeliveryCache"},
            {10101, D<&IBcatService::RequestSyncDeliveryCacheWithDirectoryName>, "RequestSyncDeliveryCacheWithDirectoryName"},
            {10200, nullptr, "CancelSyncDeliveryCacheRequest"},
            {20100, nullptr, "RequestSyncDeliveryCacheWithApplicationId"},
            {20101, nullptr, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName"},
            {20300, nullptr, "GetDeliveryCacheStorageUpdateNotifier"},
            {20301, nullptr, "RequestSuspendDeliveryTask"},
            {20400, nullptr, "RegisterSystemApplicationDeliveryTask"},
            {20401, nullptr, "UnregisterSystemApplicationDeliveryTask"},
            {20410, nullptr, "SetSystemApplicationDeliveryTaskTimer"},
            {30100, D<&IBcatService::SetPassphrase>, "SetPassphrase"},
            {30101, nullptr, "Unknown30101"},
            {30102, nullptr, "Unknown30102"},
            {30200, nullptr, "RegisterBackgroundDeliveryTask"},
            {30201, nullptr, "UnregisterBackgroundDeliveryTask"},
            {30202, nullptr, "BlockDeliveryTask"},
            {30203, nullptr, "UnblockDeliveryTask"},
            {30210, nullptr, "SetDeliveryTaskTimer"},
            {30300, D<&IBcatService::RegisterSystemApplicationDeliveryTasks>, "RegisterSystemApplicationDeliveryTasks"},
            {90100, nullptr, "EnumerateBackgroundDeliveryTask"},
            {90101, nullptr, "Unknown90101"},
            {90200, nullptr, "GetDeliveryList"},
            {90201, D<&IBcatService::ClearDeliveryCacheStorage>, "ClearDeliveryCacheStorage"},
            {90202, nullptr, "ClearDeliveryTaskSubscriptionStatus"},
            {90300, nullptr, "GetPushNotificationLog"},
            {90301, nullptr, "Unknown90301"},
        };
    // clang-format on
    RegisterHandlers(functions);
}

IBcatService::~IBcatService() = default;

Result IBcatService::RequestSyncDeliveryCache(
    OutInterface<IDeliveryCacheProgressService> out_interface) {
    LOG_DEBUG(Service_BCAT, "called");

    auto& progress_backend{GetProgressBackend(SyncType::Normal)};
    backend.Synchronize({system.GetApplicationProcessProgramID(),
                         GetCurrentBuildID(system.GetApplicationProcessBuildID())},
                        GetProgressBackend(SyncType::Normal));

    *out_interface = std::make_shared<IDeliveryCacheProgressService>(
        system, progress_backend.GetEvent(), progress_backend.GetImpl());
    R_SUCCEED();
}

Result IBcatService::RequestSyncDeliveryCacheWithDirectoryName(
    const DirectoryName& name_raw, OutInterface<IDeliveryCacheProgressService> out_interface) {
    const auto name = Common::StringFromFixedZeroTerminatedBuffer(name_raw.data(), name_raw.size());

    LOG_DEBUG(Service_BCAT, "called, name={}", name);

    auto& progress_backend{GetProgressBackend(SyncType::Directory)};
    backend.SynchronizeDirectory({system.GetApplicationProcessProgramID(),
                                  GetCurrentBuildID(system.GetApplicationProcessBuildID())},
                                 name, progress_backend);

    *out_interface = std::make_shared<IDeliveryCacheProgressService>(
        system, progress_backend.GetEvent(), progress_backend.GetImpl());
    R_SUCCEED();
}

Result IBcatService::SetPassphrase(u64 application_id,
                                   InBuffer<BufferAttr_HipcPointer> passphrase_buffer) {
    LOG_DEBUG(Service_BCAT, "called, application_id={:016X}, passphrase={}", application_id,
              Common::HexToString(passphrase_buffer));

    R_UNLESS(application_id != 0, ResultInvalidArgument);
    R_UNLESS(passphrase_buffer.size() <= 0x40, ResultInvalidArgument);

    Passphrase passphrase{};
    std::memcpy(passphrase.data(), passphrase_buffer.data(),
                std::min(passphrase.size(), passphrase_buffer.size()));

    backend.SetPassphrase(application_id, passphrase);
    R_SUCCEED();
}

Result IBcatService::RegisterSystemApplicationDeliveryTasks() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result IBcatService::ClearDeliveryCacheStorage(u64 application_id) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}", application_id);

    R_UNLESS(application_id != 0, ResultInvalidArgument);
    R_UNLESS(backend.Clear(application_id), FileSys::ResultPermissionDenied);
    R_SUCCEED();
}

ProgressServiceBackend& IBcatService::GetProgressBackend(SyncType type) {
    return progress.at(static_cast<size_t>(type));
}

const ProgressServiceBackend& IBcatService::GetProgressBackend(SyncType type) const {
    return progress.at(static_cast<size_t>(type));
}

} // namespace Service::BCAT
