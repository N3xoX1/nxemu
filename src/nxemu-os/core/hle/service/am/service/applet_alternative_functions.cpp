// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/service/applet_alternative_functions.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::AM {

IAppletAlternativeFunctions::IAppletAlternativeFunctions(Core::System& system_)
    : ServiceFramework{system_, "IAppletAlternativeFunctions"} {
    static const FunctionInfo functions[] = {
        {0, &IAppletAlternativeFunctions::Stub, "Unknown0"},
        {1, &IAppletAlternativeFunctions::Stub, "Unknown1"},
        {2, &IAppletAlternativeFunctions::Stub, "Unknown2"},
    };
    RegisterHandlers(functions);
}

IAppletAlternativeFunctions::~IAppletAlternativeFunctions() = default;

void IAppletAlternativeFunctions::Stub(HLERequestContext& ctx) {
    LOG_DEBUG(Service_AM, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

} // namespace Service::AM
