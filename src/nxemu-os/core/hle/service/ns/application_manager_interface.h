// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Service::NS {

class IApplicationManagerInterface final : public ServiceFramework<IApplicationManagerInterface> {
public:
    explicit IApplicationManagerInterface(Core::System& system_);
    ~IApplicationManagerInterface() override;

    Result GetApplicationControlData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     Out<u32> out_actual_size,
                                     ApplicationControlSource application_control_source,
                                     u64 application_id);
    Result GetApplicationDesiredLanguage(Out<ApplicationLanguage> out_desired_language,
                                         u32 supported_languages);
    Result ConvertApplicationLanguageToLanguageCode(Out<u64> out_language_code,
                                                    ApplicationLanguage application_language);
    Result ListApplicationRecord(OutArray<ApplicationRecord, BufferAttr_HipcMapAlias> out_records,
                                 Out<s32> out_count, s32 offset);
    Result GetApplicationRecordUpdateSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetGameCardMountFailureEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result IsAnyApplicationEntityInstalled(Out<bool> out_is_any_application_entity_installed);
    Result GetApplicationView(
        OutArray<ApplicationView, BufferAttr_HipcMapAlias> out_application_views,
        InArray<u64, BufferAttr_HipcMapAlias> application_ids);
    Result GetApplicationViewWithPromotionInfo(
        OutArray<ApplicationViewWithPromotionInfo, BufferAttr_HipcMapAlias> out_application_views,
        InArray<u64, BufferAttr_HipcMapAlias> application_ids);
    Result GetApplicationRightsOnClient(
        OutArray<ApplicationRightsOnClient, BufferAttr_HipcMapAlias> out_rights, Out<u32> out_count,
        u32 flags, u64 application_id, Uid account_id);
    Result CheckSdCardMountStatus();
    Result GetTotalSpaceSize(Out<s64> out_total_space_size, u64 storage_id);
    Result GetFreeSpaceSize(Out<s64> out_free_space_size, u64 storage_id);
    Result GetSdCardMountStatusChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetGameCardUpdateDetectionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result ResumeAll();
    Result TouchApplication(u64 application_id);
    Result IsApplicationUpdateRequested(Out<bool> out_update_required, Out<u32> out_update_version,
                                        u64 application_id);
    Result CheckApplicationLaunchVersion(u64 application_id);
    Result GetApplicationTerminateResult(Out<Result> out_result, u64 application_id);
};

} // namespace Service::NS
