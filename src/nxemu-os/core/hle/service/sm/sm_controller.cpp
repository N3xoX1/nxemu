// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm_controller.h"

namespace Service::SM {

constexpr Result ResultTargetNotDomain{ErrorModule::HIPC, 491};
constexpr Result ResultDomainObjectNotFound{ErrorModule::HIPC, 492};

void Controller::ConvertCurrentObjectToDomain(HLERequestContext& ctx) {
    ASSERT_MSG(!ctx.GetManager()->IsDomain(), "Session is already a domain");
    LOG_DEBUG(Service, "called, server_session={}", ctx.Session()->GetId());
    ctx.GetManager()->ConvertToDomainOnRequestEnd();

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(1); // Converted sessions start with 1 request handler
}

void Controller::CopyFromCurrentDomain(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    auto manager = ctx.GetManager();
    if (!manager->IsDomain()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultTargetNotDomain);
        return;
    }

    IPC::RequestParser rp{ctx};
    const u32 object_id = rp.PopRaw<u32>();
    if (object_id == 0 || object_id > manager->DomainHandlerCount()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultDomainObjectNotFound);
        return;
    }

    auto object = manager->DomainHandler(object_id - 1).lock();
    if (!object) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultDomainObjectNotFound);
        return;
    }

    // The copied handle is a regular session rooted at the selected domain object.
    ctx.AddMoveInterface(std::move(object));

    IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
    rb.Push(ResultSuccess);
}

void Controller::CloneCurrentObject(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    auto session_manager = ctx.GetManager();

    // FIXME: this is duplicated from the SVC, it should just call it instead
    // once this is a proper process

    // Reserve a new session from the process resource limit.
    Kernel::KScopedResourceReservation session_reservation(
        Kernel::GetCurrentProcessPointer(kernel), Kernel::LimitableResource::SessionCountMax);
    ASSERT(session_reservation.Succeeded());

    // Create the session.
    Kernel::KSession* session = Kernel::KSession::Create(kernel);
    ASSERT(session != nullptr);

    // Initialize the session.
    session->Initialize(nullptr, 0);

    // Commit the session reservation.
    session_reservation.Commit();

    // Register the session.
    Kernel::KSession::Register(kernel, session);

    // Register with server manager.
    session_manager->GetServerManager().RegisterSession(&session->GetServerSession(),
                                                        session_manager);

    // We succeeded.
    IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
    rb.Push(ResultSuccess);
    rb.PushMoveObjects(session->GetClientSession());
}

void Controller::CloneCurrentObjectEx(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    CloneCurrentObject(ctx);
}

void Controller::QueryPointerBufferSize(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    // HLE services share a process, so report the standard 32 KiB pointer buffer.
    constexpr u16 pointer_buffer_size = 0x8000;

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u16>(pointer_buffer_size);
}

// https://switchbrew.org/wiki/IPC_Marshalling
Controller::Controller(Core::System& system_) : ServiceFramework{system_, "IpcController"} {
    static const FunctionInfo functions[] = {
        {0, &Controller::ConvertCurrentObjectToDomain, "ConvertCurrentObjectToDomain"},
        {1, &Controller::CopyFromCurrentDomain, "CopyFromCurrentDomain"},
        {2, &Controller::CloneCurrentObject, "CloneCurrentObject"},
        {3, &Controller::QueryPointerBufferSize, "QueryPointerBufferSize"},
        {4, &Controller::CloneCurrentObjectEx, "CloneCurrentObjectEx"},
    };
    RegisterHandlers(functions);
}

Controller::~Controller() = default;

} // namespace Service::SM
