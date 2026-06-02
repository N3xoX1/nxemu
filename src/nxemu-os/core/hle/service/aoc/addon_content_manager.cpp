// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <numeric>
#include <vector>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/aoc/addon_content_manager.h"
#include "core/hle/service/aoc/purchase_event_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include <nxemu-module-spec/system_loader.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/logging/log.h>
#include <yuzu_common/settings.h>

namespace FileSys {

    constexpr u64 AOC_TITLE_ID_MASK = 0x7FF;
    constexpr u64 BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

    /**
     * Gets the base title ID from a given title ID.
     *
     * @param title_id The title ID.
     * @returns The base title ID.
     */
    [[nodiscard]] constexpr u64 GetBaseTitleID(u64 title_id) {
        return title_id & BASE_TITLE_ID_MASK;
    }

    /**
     * Gets the AOC (Add-On Content) ID from a given AOC title ID.
     *
     * @param aoc_title_id The AOC title ID.
     * @returns The AOC ID.
     */
    [[nodiscard]] constexpr u64 GetAOCID(u64 aoc_title_id) {
        return aoc_title_id & AOC_TITLE_ID_MASK;
    }
}

namespace Service::AOC {

static bool CheckAOCTitleIDMatchesBase(u64 title_id, u64 base) {
    return FileSys::GetBaseTitleID(title_id) == base;
}

static std::vector<u64> AccumulateAOCTitleIDs(Core::System & system) {
    std::vector<u64> add_on_content;
    ISystemloader & loader = system.GetSystemloader();
    uint32_t entriesCount = loader.GetContentProviderEntriesCount(true, LoaderTitleType::AOC, true, LoaderContentRecordType::Data, false, 0);
    
    if (entriesCount > 0)
    {
        std::vector<ContentProviderEntry> entries(entriesCount);
        entriesCount = loader.GetContentProviderEntries(true, LoaderTitleType::AOC, true, LoaderContentRecordType::Data, false, 0, entries.data(), entriesCount);
        entries.resize(entriesCount);
        std::transform(entries.begin(), entries.end(), std::back_inserter(add_on_content),
            [](const ContentProviderEntry& rce) { return rce.titleID; });

        add_on_content.erase(
            std::remove_if(
                add_on_content.begin(), add_on_content.end(),
                [&loader](u64 tid) {
                    return FileSysNCAPtr(loader.GetContentProviderEntry(tid, LoaderContentRecordType::Data))->GetStatus() != LoaderResultStatus::Success;
                }),
            add_on_content.end());
    }
    return add_on_content;
}

IAddOnContentManager::IAddOnContentManager(Core::System& system_)
    : ServiceFramework{system_, "aoc:u"}, add_on_content{AccumulateAOCTitleIDs(system)},
      service_context{system_, "aoc:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CountAddOnContentByApplicationId"},
        {1, nullptr, "ListAddOnContentByApplicationId"},
        {2, D<&IAddOnContentManager::CountAddOnContent>, "CountAddOnContent"},
        {3, D<&IAddOnContentManager::ListAddOnContent>, "ListAddOnContent"},
        {4, nullptr, "GetAddOnContentBaseIdByApplicationId"},
        {5, D<&IAddOnContentManager::GetAddOnContentBaseId>, "GetAddOnContentBaseId"},
        {6, nullptr, "PrepareAddOnContentByApplicationId"},
        {7, D<&IAddOnContentManager::PrepareAddOnContent>, "PrepareAddOnContent"},
        {8, D<&IAddOnContentManager::GetAddOnContentListChangedEvent>, "GetAddOnContentListChangedEvent"},
        {9, nullptr, "GetAddOnContentLostErrorCode"},
        {10, D<&IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId>, "GetAddOnContentListChangedEventWithProcessId"},
        {11, D<&IAddOnContentManager::NotifyMountAddOnContent>, "NotifyMountAddOnContent"},
        {12, D<&IAddOnContentManager::NotifyUnmountAddOnContent>, "NotifyUnmountAddOnContent"},
        {13, nullptr, "IsAddOnContentMountedForDebug"},
        {50, D<&IAddOnContentManager::CheckAddOnContentMountStatus>, "CheckAddOnContentMountStatus"},
        {100, D<&IAddOnContentManager::CreateEcPurchasedEventManager>, "CreateEcPurchasedEventManager"},
        {101, D<&IAddOnContentManager::CreatePermanentEcPurchasedEventManager>, "CreatePermanentEcPurchasedEventManager"},
        {110, nullptr, "CreateContentsServiceManager"},
        {200, nullptr, "SetRequiredAddOnContentsOnContentsAvailabilityTransition"},
        {300, nullptr, "SetupHostAddOnContent"},
        {301, nullptr, "GetRegisteredAddOnContentPath"},
        {302, nullptr, "UpdateCachedList"},
    };
    // clang-format on

    RegisterHandlers(functions);

    aoc_change_event = service_context.CreateEvent("GetAddOnContentListChanged:Event");
}

IAddOnContentManager::~IAddOnContentManager() {
    service_context.CloseEvent(aoc_change_event);
}

Result IAddOnContentManager::CountAddOnContent(Out<u32> out_count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called. process_id={}", process_id.pid);

    const auto current = system.GetApplicationProcessProgramID();

    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") != disabled.end()) {
        *out_count = 0;
        R_SUCCEED();
    }

    *out_count = static_cast<u32>(
        std::count_if(add_on_content.begin(), add_on_content.end(),
                      [current](u64 tid) { return CheckAOCTitleIDMatchesBase(tid, current); }));

    R_SUCCEED();
}

Result IAddOnContentManager::ListAddOnContent(Out<u32> out_count,
                                              OutBuffer<BufferAttr_HipcMapAlias> out_addons,
                                              u32 offset, u32 count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called with offset={}, count={}, process_id={}", offset, count,
              process_id.pid);

    const auto current = FileSys::GetBaseTitleID(system.GetApplicationProcessProgramID());

    std::vector<u32> out;
    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") == disabled.end()) {
        for (u64 content_id : add_on_content) {
            if (FileSys::GetBaseTitleID(content_id) != current) {
                continue;
            }

            out.push_back(static_cast<u32>(FileSys::GetAOCID(content_id)));
        }
    }

    // TODO(DarkLordZach): Find the correct error code.
    R_UNLESS(out.size() >= offset, ResultUnknown);

    *out_count = static_cast<u32>(std::min<size_t>(out.size() - offset, count));
    std::rotate(out.begin(), out.begin() + offset, out.end());

    std::memcpy(out_addons.data(), out.data(), *out_count * sizeof(u32));

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentBaseId(Out<u64> out_title_id,
                                                   ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called, process_id={}", process_id.pid);

    const u64 application_id = system.GetApplicationProcessProgramID();
    *out_title_id = FileSys::GetBaseTitleID(application_id) | 0x1000;

    R_SUCCEED();
}

Result IAddOnContentManager::PrepareAddOnContent(s32 addon_index, ClientProcessId process_id) {
    LOG_WARNING(Service_AOC, "(STUBBED) called with addon_index={}, process_id={}", addon_index,
                process_id.pid);

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId(
    OutCopyHandle<Kernel::KReadableEvent> out_event, ClientProcessId process_id) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyMountAddOnContent() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyUnmountAddOnContent() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::CheckAddOnContentMountStatus() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::CreateEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

Result IAddOnContentManager::CreatePermanentEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    server_manager->RegisterNamedService("aoc:u", std::make_shared<IAddOnContentManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::AOC
