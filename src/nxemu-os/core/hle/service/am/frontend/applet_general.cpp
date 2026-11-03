// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/frontend/applet_general.h"
#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/service/storage.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"

namespace Service::AM::Frontend
{

constexpr Result ERROR_INVALID_PIN{ErrorModule::PCTL, 221};

static void LogCurrentStorage(std::shared_ptr<Applet> applet, std::string_view prefix)
{
    std::shared_ptr<IStorage> storage;
    while (R_SUCCEEDED(applet->caller_applet_broker->GetInData().Pop(&storage)))
    {
        const auto data = storage->GetData();
        LOG_INFO(Service_AM,
                 "called (STUBBED), during {} received normal data with size={:08X}, data={}",
                 prefix, data.size(), Common::HexToString(data));
    }

    while (R_SUCCEEDED(applet->caller_applet_broker->GetInteractiveInData().Pop(&storage)))
    {
        const auto data = storage->GetData();
        LOG_INFO(Service_AM,
                 "called (STUBBED), during {} received interactive data with size={:08X}, data={}",
                 prefix, data.size(), Common::HexToString(data));
    }
}

Auth::Auth(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_) :
    FrontendApplet{system_, applet_, applet_mode_}
{
}

Auth::~Auth() = default;

void Auth::Initialize()
{
    FrontendApplet::Initialize();
    complete = false;

    const std::shared_ptr<IStorage> storage = PopInData();
    ASSERT(storage != nullptr);
    const auto data = storage->GetData();
    ASSERT(data.size() >= 0xC);

    struct Arg
    {
        INSERT_PADDING_BYTES(4);
        AuthAppletType type;
        u8 arg0;
        u8 arg1;
        u8 arg2;
        INSERT_PADDING_BYTES(1);
    };
    static_assert(sizeof(Arg) == 0xC, "Arg (AuthApplet) has incorrect size.");

    Arg arg{};
    std::memcpy(&arg, data.data(), sizeof(Arg));

    type = arg.type;
    arg0 = arg.arg0;
    arg1 = arg.arg1;
    arg2 = arg.arg2;
}

Result Auth::GetStatus() const
{
    return successful ? ResultSuccess : ERROR_INVALID_PIN;
}

void Auth::ExecuteInteractive()
{
    ASSERT_MSG(false, "Unexpected interactive applet data.");
}

void Auth::Execute()
{
    if (complete)
    {
        return;
    }
    UNIMPLEMENTED();
}

void Auth::AuthFinished(bool is_successful)
{
    successful = is_successful;

    struct Return
    {
        Result result_code;
    };
    static_assert(sizeof(Return) == 0x4, "Return (AuthApplet) has incorrect size.");

    Return return_{GetStatus()};

    std::vector<u8> out(sizeof(Return));
    std::memcpy(out.data(), &return_, sizeof(Return));

    PushOutData(std::make_shared<IStorage>(system, std::move(out)));
    Exit();
}

Result Auth::RequestExit()
{
    UNIMPLEMENTED();
    R_SUCCEED();
}

PhotoViewer::PhotoViewer(Core::System & system_, std::shared_ptr<Applet> applet_,
                         LibraryAppletMode applet_mode_) :
    FrontendApplet{system_, applet_, applet_mode_}
{
}

PhotoViewer::~PhotoViewer() = default;

void PhotoViewer::Initialize()
{
    FrontendApplet::Initialize();
    complete = false;

    const std::shared_ptr<IStorage> storage = PopInData();
    ASSERT(storage != nullptr);
    const auto data = storage->GetData();
    ASSERT(!data.empty());
    mode = static_cast<PhotoViewerAppletMode>(data[0]);
}

Result PhotoViewer::GetStatus() const
{
    return ResultSuccess;
}

void PhotoViewer::ExecuteInteractive()
{
    ASSERT_MSG(false, "Unexpected interactive applet data.");
}

void PhotoViewer::Execute()
{
    UNIMPLEMENTED();
}

void PhotoViewer::ViewFinished()
{
    PushOutData(std::make_shared<IStorage>(system, std::vector<u8>{}));
    Exit();
}

Result PhotoViewer::RequestExit()
{
    UNIMPLEMENTED();
    R_SUCCEED();
}

Result NetConnect::GetStatus() const { return ResultSuccess; }

void NetConnect::ExecuteInteractive() { Execute(); }

void NetConnect::Execute() {
    if (complete) {
        return;
    }
    complete = true;
    // nifmLa expects a status word followed by the connection Result.
    const std::array<u32, 2> result{0, Result{ErrorModule::NIFM, 1111}.raw};
    std::vector<u8> data(sizeof(result));
    std::memcpy(data.data(), result.data(), sizeof(result));
    PushOutData(std::make_shared<IStorage>(system, std::move(data)));
    LOG_INFO(Service_AM, "Network connection applet completed with network communication disabled");
    Exit();
}

Result NetConnect::RequestExit() { Execute(); return ResultSuccess; }

StubApplet::StubApplet(Core::System & system_, std::shared_ptr<Applet> applet_, AppletId id_,
                       LibraryAppletMode applet_mode_) :
    FrontendApplet{system_, applet_, applet_mode_}, id{id_}
{
}

StubApplet::~StubApplet() = default;

void StubApplet::Initialize()
{
    LOG_WARNING(Service_AM, "called (STUBBED)");
    FrontendApplet::Initialize();

    LogCurrentStorage(applet.lock(), "Initialize");
}

Result StubApplet::GetStatus() const
{
    LOG_WARNING(Service_AM, "called (STUBBED)");
    return ResultSuccess;
}

void StubApplet::ExecuteInteractive()
{
    LOG_WARNING(Service_AM, "called (STUBBED)");
    LogCurrentStorage(applet.lock(), "ExecuteInteractive");

    PushOutData(std::make_shared<IStorage>(system, std::vector<u8>(0x1000)));
    PushInteractiveOutData(std::make_shared<IStorage>(system, std::vector<u8>(0x1000)));
    Exit();
}

void StubApplet::Execute()
{
    LOG_WARNING(Service_AM, "called (STUBBED)");
    LogCurrentStorage(applet.lock(), "Execute");

    PushOutData(std::make_shared<IStorage>(system, std::vector<u8>(0x1000)));
    PushInteractiveOutData(std::make_shared<IStorage>(system, std::vector<u8>(0x1000)));
    Exit();
}

Result StubApplet::RequestExit()
{
    // Nothing to do.
    R_SUCCEED();
}

} // namespace Service::AM::Frontend
