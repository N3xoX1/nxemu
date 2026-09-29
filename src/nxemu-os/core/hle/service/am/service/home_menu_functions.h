// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::AM {

struct Applet;
class IStorage;

class IHomeMenuFunctions final : public ServiceFramework<IHomeMenuFunctions> {
public:
    explicit IHomeMenuFunctions(Core::System& system_, std::shared_ptr<Applet> applet);
    ~IHomeMenuFunctions() override;

private:
    Result RequestToGetForeground();
    Result LockForeground();
    Result UnlockForeground();
    Result PopFromGeneralChannel(Out<SharedPointer<IStorage>> out_storage);
    Result GetPopFromGeneralChannelEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result IsSleepEnabled(Out<bool> out_is_sleep_enabled);
    Result IsRebootEnabled(Out<bool> out_is_reboot_enbaled);
    Result IsForceTerminateApplicationDisabledForDebug(
        Out<bool> out_is_force_terminate_application_disabled_for_debug);

    const std::shared_ptr<Applet> m_applet;
};

} // namespace Service::AM
