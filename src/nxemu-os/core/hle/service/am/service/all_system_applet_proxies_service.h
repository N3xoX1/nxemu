// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service {

namespace AM {

struct Applet;
struct AppletAttribute;
class IAppletAlternativeFunctions;
class IApplicationProxy;
class ILibraryAppletProxy;
class IOverlayAppletProxy;
class ISystemAppletProxy;
class ISystemProcessCommonFunctions;

class IAllSystemAppletProxiesService final
    : public ServiceFramework<IAllSystemAppletProxiesService> {
public:
    explicit IAllSystemAppletProxiesService(Core::System& system_);
    ~IAllSystemAppletProxiesService() override;

private:
    Result OpenSystemAppletProxy(Out<SharedPointer<ISystemAppletProxy>> out_system_applet_proxy,
                                 ClientProcessId pid,
                                 InCopyHandle<Kernel::KProcess> process_handle);
    Result OpenSystemAppletProxyEx(
        Out<SharedPointer<ISystemAppletProxy>> out_system_applet_proxy, ClientProcessId pid,
        InCopyHandle<Kernel::KProcess> process_handle,
        InLargeData<AppletAttribute, BufferAttr_HipcMapAlias> attribute);
    Result OpenOverlayAppletProxy(Out<SharedPointer<IOverlayAppletProxy>> out_overlay_applet_proxy,
                                  ClientProcessId pid,
                                  InCopyHandle<Kernel::KProcess> process_handle);
    Result GetSystemProcessCommonFunctions(
        Out<SharedPointer<ISystemProcessCommonFunctions>> out_system_process_common_functions);
    Result GetAppletAlternativeFunctions(
        Out<SharedPointer<IAppletAlternativeFunctions>> out_applet_alternative_functions);
    Result OpenSystemApplicationProxy(
        Out<SharedPointer<IApplicationProxy>> out_system_application_proxy, ClientProcessId pid,
        InCopyHandle<Kernel::KProcess> process_handle);
    Result OpenLibraryAppletProxy(Out<SharedPointer<ILibraryAppletProxy>> out_library_applet_proxy,
                                  ClientProcessId pid,
                                  InCopyHandle<Kernel::KProcess> process_handle,
                                  InLargeData<AppletAttribute, BufferAttr_HipcMapAlias> attribute);
    Result OpenLibraryAppletProxyOld(
        Out<SharedPointer<ILibraryAppletProxy>> out_library_applet_proxy, ClientProcessId pid,
        InCopyHandle<Kernel::KProcess> process_handle);

private:
    std::shared_ptr<Applet> GetAppletFromProcessId(ProcessId pid);
};

} // namespace AM
} // namespace Service
