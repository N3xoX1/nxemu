// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/library_applet_storage.h"
#include "core/hle/service/am/service/library_applet_accessor.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/sm/sm.h"
#include "os_settings.h"
#include "os_types.h"

namespace Service::AM
{

namespace
{

bool ShouldCreateGuestApplet(AppletId applet_id)
{
    switch (applet_id)
    {
    case AppletId::Cabinet: return osSettings.cabinet_applet_mode == AppletMode::LLE;
    case AppletId::Controller: return osSettings.controller_applet_mode == AppletMode::LLE;
    case AppletId::DataErase: return osSettings.data_erase_applet_mode == AppletMode::LLE;
    case AppletId::Error: return osSettings.error_applet_mode == AppletMode::LLE;
    case AppletId::NetConnect: return osSettings.net_connect_applet_mode == AppletMode::LLE;
    case AppletId::ProfileSelect: return osSettings.player_select_applet_mode == AppletMode::LLE;
    case AppletId::SoftwareKeyboard: return osSettings.swkbd_applet_mode == AppletMode::LLE;
    case AppletId::MiiEdit: return osSettings.mii_edit_applet_mode == AppletMode::LLE;
    case AppletId::Web: return osSettings.web_applet_mode == AppletMode::LLE;
    case AppletId::Shop: return osSettings.shop_applet_mode == AppletMode::LLE;
    case AppletId::PhotoViewer: return osSettings.photo_viewer_applet_mode == AppletMode::LLE;
    case AppletId::OfflineWeb: return osSettings.offline_web_applet_mode == AppletMode::LLE;
    case AppletId::LoginShare: return osSettings.login_share_applet_mode == AppletMode::LLE;
    case AppletId::WebAuth: return osSettings.wifi_web_auth_applet_mode == AppletMode::LLE;
    case AppletId::MyPage: return osSettings.my_page_applet_mode == AppletMode::LLE;
    default: return true;
    }
}

AppletProgramId AppletIdToProgramId(AppletId applet_id)
{
    switch (applet_id)
    {
    case AppletId::OverlayDisplay:
        return AppletProgramId::OverlayDisplay;
    case AppletId::QLaunch:
        return AppletProgramId::QLaunch;
    case AppletId::Starter:
        return AppletProgramId::Starter;
    case AppletId::Auth:
        return AppletProgramId::Auth;
    case AppletId::Cabinet:
        return AppletProgramId::Cabinet;
    case AppletId::Controller:
        return AppletProgramId::Controller;
    case AppletId::DataErase:
        return AppletProgramId::DataErase;
    case AppletId::Error:
        return AppletProgramId::Error;
    case AppletId::NetConnect:
        return AppletProgramId::NetConnect;
    case AppletId::ProfileSelect:
        return AppletProgramId::ProfileSelect;
    case AppletId::SoftwareKeyboard:
        return AppletProgramId::SoftwareKeyboard;
    case AppletId::MiiEdit:
        return AppletProgramId::MiiEdit;
    case AppletId::Web:
        return AppletProgramId::Web;
    case AppletId::Shop:
        return AppletProgramId::Shop;
    case AppletId::PhotoViewer:
        return AppletProgramId::PhotoViewer;
    case AppletId::Settings:
        return AppletProgramId::Settings;
    case AppletId::OfflineWeb:
        return AppletProgramId::OfflineWeb;
    case AppletId::LoginShare:
        return AppletProgramId::LoginShare;
    case AppletId::WebAuth:
        return AppletProgramId::WebAuth;
    case AppletId::MyPage:
        return AppletProgramId::MyPage;
    default:
        return static_cast<AppletProgramId>(0);
    }
}

std::shared_ptr<ILibraryAppletAccessor> CreateGuestApplet(Core::System & system, std::shared_ptr<Applet> caller_applet, AppletId applet_id, LibraryAppletMode mode)
{
    const auto program_id = static_cast<u64>(AppletIdToProgramId(applet_id));
    if (program_id == 0) 
    {
        // Unknown applet
        return {};
    }

    auto process = std::make_unique<Process>(system);
    if (!process->Initialize(program_id)) 
    {
        // Couldn't initialize the guest process
        return {};
    }

    const auto applet = std::make_shared<Applet>(system, std::move(process));
    applet->program_id = program_id;
    applet->applet_id = applet_id;
    applet->type = AppletType::LibraryApplet;
    applet->library_applet_mode = mode;

    // Set focus state
    switch (mode) {
    case LibraryAppletMode::AllForeground:
    case LibraryAppletMode::NoUi:
    case LibraryAppletMode::PartialForeground:
    case LibraryAppletMode::PartialForegroundIndirectDisplay:
        applet->hid_registration.EnableAppletToGetInput(true);
        applet->focus_state = FocusState::InFocus;
        applet->message_queue.PushMessage(AppletMessage::ChangeIntoForeground);
        break;
    case LibraryAppletMode::AllForegroundInitiallyHidden:
        applet->hid_registration.EnableAppletToGetInput(false);
        applet->focus_state = FocusState::NotInFocus;
        applet->display_layer_manager.SetWindowVisibility(false);
        applet->message_queue.PushMessage(AppletMessage::ChangeIntoBackground);
        break;
    }

    auto broker = std::make_shared<AppletDataBroker>(system);
    applet->caller_applet = caller_applet;
    applet->caller_applet_broker = broker;

    system.GetAppletManager().InsertApplet(applet);

    return std::make_shared<ILibraryAppletAccessor>(system, broker, applet);
}

std::shared_ptr<ILibraryAppletAccessor> CreateFrontendApplet(Core::System & system, std::shared_ptr<Applet> caller_applet, AppletId applet_id, LibraryAppletMode mode)
{
    const auto program_id = static_cast<u64>(AppletIdToProgramId(applet_id));

    auto process = std::make_unique<Process>(system);
    auto applet = std::make_shared<Applet>(system, std::move(process));
    applet->program_id = program_id;
    applet->applet_id = applet_id;
    applet->type = AppletType::LibraryApplet;
    applet->library_applet_mode = mode;

    auto storage = std::make_shared<AppletDataBroker>(system);
    applet->caller_applet = caller_applet;
    applet->caller_applet_broker = storage;
    applet->frontend = system.GetFrontendAppletHolder().GetApplet(applet, applet_id, mode);

    return std::make_shared<ILibraryAppletAccessor>(system, storage, applet);
}

} // namespace

ILibraryAppletCreator::ILibraryAppletCreator(Core::System & system_, std::shared_ptr<Applet> applet) :
    ServiceFramework{system_, "ILibraryAppletCreator"}, m_applet{std::move(applet)}
{
    static const FunctionInfo functions[] = {
        {0, D<&ILibraryAppletCreator::CreateLibraryApplet>, "CreateLibraryApplet"},
        {1, nullptr, "TerminateAllLibraryApplets"},
        {2, nullptr, "AreAnyLibraryAppletsLeft"},
        {10, D<&ILibraryAppletCreator::CreateStorage>, "CreateStorage"},
        {11, D<&ILibraryAppletCreator::CreateTransferMemoryStorage>, "CreateTransferMemoryStorage"},
        {12, D<&ILibraryAppletCreator::CreateHandleStorage>, "CreateHandleStorage"},
    };
    RegisterHandlers(functions);
}

ILibraryAppletCreator::~ILibraryAppletCreator() = default;

Result ILibraryAppletCreator::CreateLibraryApplet(Out<SharedPointer<ILibraryAppletAccessor>> out_library_applet_accessor, AppletId applet_id, LibraryAppletMode library_applet_mode)
{
    LOG_DEBUG(Service_AM, "called with applet_id={} applet_mode={}", applet_id, library_applet_mode);

    std::shared_ptr<ILibraryAppletAccessor> library_applet;
    if (ShouldCreateGuestApplet(applet_id))
    {
        library_applet = CreateGuestApplet(system, m_applet, applet_id, library_applet_mode);
    }
    if (!library_applet)
    {
        library_applet = CreateFrontendApplet(system, m_applet, applet_id, library_applet_mode);
    }
    if (!library_applet)
    {
        LOG_ERROR(Service_AM, "Applet doesn't exist! applet_id={}", applet_id);
        R_THROW(ResultUnknown);
    }

    // Applet is created, can now be launched.
    m_applet->library_applet_launchable_event.Signal();
    *out_library_applet_accessor = library_applet;
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateStorage(Out<SharedPointer<IStorage>> out_storage, s64 size)
{
    LOG_DEBUG(Service_AM, "called, size={}", size);

    if (size <= 0)
    {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(system, AM::CreateStorage(std::vector<u8>(size)));
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateTransferMemoryStorage(Out<SharedPointer<IStorage>> out_storage, bool is_writable, s64 size, InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle)
{
    LOG_DEBUG(Service_AM, "called, is_writable={} size={}", is_writable, size);

    if (size <= 0)
    {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    if (!transfer_memory_handle)
    {
        LOG_ERROR(Service_AM, "transfer_memory_handle is null");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(system, AM::CreateTransferMemoryStorage(transfer_memory_handle->GetOwner()->GetCoreMemory(), transfer_memory_handle.Get(), is_writable, size));
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateHandleStorage(Out<SharedPointer<IStorage>> out_storage, s64 size, InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle)
{
    LOG_DEBUG(Service_AM, "called, size={}", size);

    if (size <= 0)
    {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    if (!transfer_memory_handle)
    {
        LOG_ERROR(Service_AM, "transfer_memory_handle is null");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(system, AM::CreateHandleStorage(transfer_memory_handle->GetOwner()->GetCoreMemory(), transfer_memory_handle.Get(), size));
    R_SUCCEED();
}

} // namespace Service::AM
