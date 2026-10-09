// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Service names, scoped interfaces and command tables are adapted from Eden.
// Unsupported operations use error-only replies rather than reporting success.

#include "core/hle/service/ngc/streamplay.h"
#include <span>
#include <nxemu-module-spec/system_loader.h>
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::NGC {
namespace {

class StreamPlayShim final : public ServiceFramework<StreamPlayShim> {
public:
    StreamPlayShim(Core::System& system_, bool system_scope)
        : ServiceFramework{system_, system_scope ? "ISystemShimScopedObject" : "IUserShimScopedObject"} {
        const std::span<const StreamPlayCommand> commands = system_scope
            ? std::span<const StreamPlayCommand>{SystemShimCommands}
            : std::span<const StreamPlayCommand>{UserShimCommands};
        for (const auto& command : commands) {
            const FunctionInfo function{command.id, &StreamPlayShim::Unsupported, command.name};
            RegisterHandlers(&function, 1);
        }
    }

private:
    void Unsupported(HLERequestContext& ctx) {
        LOG_WARNING(Service_NGC, "StreamPlay operation unavailable: {} command={}",
                    GetServiceName(), ctx.GetCommand());
        // No save handles, writes, events or asynchronous objects exist yet. An
        // error-only response must not imply a successful read or durable write.
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Result{StreamPlayNotSupported});
    }
};

class StreamPlayService final : public ServiceFramework<StreamPlayService> {
public:
    StreamPlayService(Core::System& system_, bool system_scope_)
        : ServiceFramework{system_, system_scope_ ? "stpl:sys" : "stpl:u"},
          system_scope{system_scope_} {
        static const FunctionInfo functions[]{
            {0, &StreamPlayService::CreateShim, "CreateShimScopedObject"},
        };
        RegisterHandlers(functions);
    }

private:
    void CreateShim(HLERequestContext& ctx) {
        if (!system_scope) {
            IPC::RequestParser parser{ctx};
            const auto mode = parser.Pop<u32>();
            LOG_DEBUG(Service_NGC, "StreamPlay user shim requested, mode={:#x}", mode);
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<StreamPlayShim>(system, system_scope);
    }

    bool system_scope;
};

} // namespace

void RegisterStreamPlayServices(ServerManager& manager, Core::System& system) {
    char version[32]{};
    if (system.GetSystemloader().GetInstalledFirmwareDisplayVersion(version, sizeof(version)) == 0 ||
        !IsStreamPlayFirmwareSupported(version)) {
        return;
    }
    manager.RegisterNamedService("stpl:u", std::make_shared<StreamPlayService>(system, false), 4);
    manager.RegisterNamedService("stpl:sys", std::make_shared<StreamPlayService>(system, true), 4);
}

} // namespace Service::NGC
