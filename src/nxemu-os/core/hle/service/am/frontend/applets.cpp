// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "applets/cabinet.h"
#include "applets/controller.h"
#include "applets/error.h"
#include "applets/general.h"
#include "applets/mii_edit.h"
#include "applets/profile_select.h"
#include "applets/software_keyboard.h"
#include "applets/web_browser.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applet_cabinet.h"
#include "core/hle/service/am/frontend/applet_controller.h"
#include "core/hle/service/am/frontend/applet_error.h"
#include "core/hle/service/am/frontend/applet_general.h"
#include "core/hle/service/am/frontend/applet_mii_edit.h"
#include "core/hle/service/am/frontend/applet_profile_select.h"
#include "core/hle/service/am/frontend/applet_software_keyboard.h"
#include "core/hle/service/am/frontend/applet_web_browser.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/sm/sm.h"
#include "yuzu_common/yuzu_assert.h"

namespace Service::AM::Frontend
{

struct FrontendAppletHolder::DefaultApplets
{
    std::unique_ptr<DefaultCabinetApplet> cabinet;
    std::unique_ptr<DefaultControllerApplet> controller;
    std::unique_ptr<DefaultErrorApplet> error;
    std::unique_ptr<DefaultMiiEditApplet> mii_edit;
    std::unique_ptr<DefaultParentalControlsApplet> parental_controls;
    std::unique_ptr<DefaultPhotoViewerApplet> photo_viewer;
    std::unique_ptr<DefaultProfileSelectApplet> profile_select;
    std::unique_ptr<DefaultSoftwareKeyboardApplet> software_keyboard;
    std::unique_ptr<DefaultWebBrowserApplet> web_browser;
};

FrontendApplet::FrontendApplet(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_) :
    system{system_},
    applet{std::move(applet_)},
    applet_mode{applet_mode_}
{
}

FrontendApplet::~FrontendApplet() = default;

void FrontendApplet::Initialize()
{
    std::shared_ptr<IStorage> common = PopInData();
    ASSERT(common != nullptr);
    const auto common_data = common->GetData();

    ASSERT(common_data.size() >= sizeof(CommonArguments));
    std::memcpy(&common_args, common_data.data(), sizeof(CommonArguments));

    initialized = true;
}

std::shared_ptr<IStorage> FrontendApplet::PopInData()
{
    std::shared_ptr<IStorage> ret;
    applet.lock()->caller_applet_broker->GetInData().Pop(&ret);
    return ret;
}

std::shared_ptr<IStorage> FrontendApplet::PopInteractiveInData()
{
    std::shared_ptr<IStorage> ret;
    applet.lock()->caller_applet_broker->GetInteractiveInData().Pop(&ret);
    return ret;
}

void FrontendApplet::PushOutData(std::shared_ptr<IStorage> storage)
{
    applet.lock()->caller_applet_broker->GetOutData().Push(storage);
}

void FrontendApplet::PushInteractiveOutData(std::shared_ptr<IStorage> storage)
{
    applet.lock()->caller_applet_broker->GetInteractiveOutData().Push(storage);
}

void FrontendApplet::Exit()
{
    applet.lock()->caller_applet_broker->SignalCompletion();
}

FrontendAppletSet::FrontendAppletSet() :
    cabinet(nullptr),
    controller(nullptr),
    error(nullptr),
    mii_edit(nullptr),
    parental_controls(nullptr),
    photo_viewer(nullptr),
    profile_select(nullptr),
    software_keyboard(nullptr),
    web_browser(nullptr)
{
}

FrontendAppletSet::FrontendAppletSet(ICabinetApplet * cabinet_, IControllerApplet * controller_, IErrorApplet * error_, IMiiEditApplet * mii_edit_, IParentalControlsApplet * parental_controls, IPhotoViewerApplet * photo_viewer, IProfileSelectApplet * profile_select_, ISoftwareKeyboardApplet * software_keyboard_, IWebBrowserApplet * web_browser_) :
    cabinet(cabinet_),
    controller(controller_),
    error(error_),
    mii_edit(mii_edit_),
    parental_controls(parental_controls),
    photo_viewer(photo_viewer),
    profile_select(profile_select_),
    software_keyboard(software_keyboard_),
    web_browser(web_browser_)
{
}

FrontendAppletSet::~FrontendAppletSet() = default;

FrontendAppletSet::FrontendAppletSet(FrontendAppletSet &&) noexcept = default;

FrontendAppletSet & FrontendAppletSet::operator=(FrontendAppletSet &&) noexcept = default;

FrontendAppletHolder::FrontendAppletHolder(Core::System & system_) :
    system{system_}
{
}

FrontendAppletHolder::~FrontendAppletHolder() = default;

const FrontendAppletSet & FrontendAppletHolder::GetFrontendAppletSet() const
{
    return frontend;
}

NFP::CabinetMode FrontendAppletHolder::GetCabinetMode() const
{
    return cabinet_mode;
}

AppletId FrontendAppletHolder::GetCurrentAppletId() const
{
    return current_applet_id;
}

void FrontendAppletHolder::SetFrontendAppletSet(FrontendAppletSet set)
{
    frontend = std::move(set);
}

void FrontendAppletHolder::SetCabinetMode(NFP::CabinetMode mode)
{
    cabinet_mode = mode;
}

void FrontendAppletHolder::SetCurrentAppletId(AppletId applet_id)
{
    current_applet_id = applet_id;
}

void FrontendAppletHolder::SetDefaultAppletsIfMissing()
{
    frontendDefaults = std::make_unique<DefaultApplets>();
    if (frontend.cabinet == nullptr)
    {
        frontendDefaults->cabinet = std::make_unique<DefaultCabinetApplet>();
        frontend.cabinet = frontendDefaults->cabinet.get();
    }
    if (frontend.controller == nullptr)
    {
        frontendDefaults->controller = std::make_unique<DefaultControllerApplet>(system.HIDCore());
        frontend.controller = frontendDefaults->controller.get();
    }
    if (frontend.error == nullptr)
    {
        frontendDefaults->error = std::make_unique<DefaultErrorApplet>();
        frontend.error = frontendDefaults->error.get();
    }
    if (frontend.mii_edit == nullptr)
    {
        frontendDefaults->mii_edit = std::make_unique<DefaultMiiEditApplet>();
        frontend.mii_edit = frontendDefaults->mii_edit.get();
    }
    if (frontend.parental_controls == nullptr)
    {
        frontendDefaults->parental_controls = std::make_unique<DefaultParentalControlsApplet>();
        frontend.parental_controls = frontendDefaults->parental_controls.get();
    }
    if (frontend.photo_viewer == nullptr)
    {
        frontendDefaults->photo_viewer = std::make_unique<DefaultPhotoViewerApplet>();
        frontend.photo_viewer = frontendDefaults->photo_viewer.get();
    }
    if (frontend.profile_select == nullptr)
    {
        frontendDefaults->profile_select = std::make_unique<DefaultProfileSelectApplet>();
        frontend.profile_select = frontendDefaults->profile_select.get();
    }
    if (frontend.software_keyboard == nullptr)
    {
        frontendDefaults->software_keyboard = std::make_unique<DefaultSoftwareKeyboardApplet>();
        frontend.software_keyboard = frontendDefaults->software_keyboard.get();
    }
    if (frontend.web_browser == nullptr)
    {
        frontendDefaults->web_browser = std::make_unique<DefaultWebBrowserApplet>();
        frontend.web_browser = frontendDefaults->web_browser.get();
    }
}

void FrontendAppletHolder::ClearAll()
{
    frontend = {};
}

std::shared_ptr<FrontendApplet> FrontendAppletHolder::GetApplet(std::shared_ptr<Applet> applet, AppletId id, LibraryAppletMode mode) const
{
    switch (id)
    {
    case AppletId::Auth:
        return std::make_shared<Auth>(system, applet, mode);
    case AppletId::Cabinet:
        return std::make_shared<Cabinet>(system, applet, mode);
    case AppletId::Controller:
        return std::make_shared<Controller>(system, applet, mode, *frontend.controller);
    case AppletId::Error:
        return std::make_shared<Error>(system, applet, mode);
    case AppletId::ProfileSelect:
        return std::make_shared<ProfileSelect>(system, applet, mode, *frontend.profile_select);
    case AppletId::SoftwareKeyboard:
        return std::make_shared<SoftwareKeyboard>(system, applet, mode, *frontend.software_keyboard);
    case AppletId::MiiEdit:
        return std::make_shared<MiiEdit>(system, applet, mode, *frontend.mii_edit);
    case AppletId::Web:
    case AppletId::Shop:
    case AppletId::OfflineWeb:
    case AppletId::LoginShare:
    case AppletId::WebAuth:
        return std::make_shared<WebBrowser>(system, applet, mode, *frontend.web_browser);
    case AppletId::PhotoViewer:
        return std::make_shared<PhotoViewer>(system, applet, mode);
    default:
        UNIMPLEMENTED_MSG("No backend implementation exists for applet_id={:02X}! Falling back to stub applet.",static_cast<u8>(id));
        return std::make_shared<StubApplet>(system, applet, id, mode);
    }
}

} // namespace Service::AM::Frontend
