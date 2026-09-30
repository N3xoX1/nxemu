// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/service/system_process_common_functions.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::AM {

IApplicationObserver::IApplicationObserver(Core::System& system_)
    : ServiceFramework{system_, "IApplicationObserver"} {
    static const FunctionInfo functions[] = {
        {1, &IApplicationObserver::Stub, "Unknown1"},
        {2, &IApplicationObserver::Stub, "Unknown2"},
        {10, &IApplicationObserver::Stub, "Unknown10"},
        {20, &IApplicationObserver::Stub, "Unknown20"},
        {30, &IApplicationObserver::Stub, "Unknown30"},
    };
    RegisterHandlers(functions);
}

IApplicationObserver::~IApplicationObserver() = default;

void IApplicationObserver::Stub(HLERequestContext& ctx) {
    LOG_DEBUG(Service_AM, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

ISystemProcessCommonFunctions::ISystemProcessCommonFunctions(Core::System& system_)
    : ServiceFramework{system_, "ISystemProcessCommonFunctions"} {
    static const FunctionInfo functions[] = {
        {1, D<&ISystemProcessCommonFunctions::GetApplicationObserver>, "GetApplicationObserver"},
    };
    RegisterHandlers(functions);
}

ISystemProcessCommonFunctions::~ISystemProcessCommonFunctions() = default;

Result ISystemProcessCommonFunctions::GetApplicationObserver(
    Out<SharedPointer<IApplicationObserver>> out_application_observer) {
    LOG_DEBUG(Service_AM, "called");
    *out_application_observer = std::make_shared<IApplicationObserver>(system);
    R_SUCCEED();
}

} // namespace Service::AM
