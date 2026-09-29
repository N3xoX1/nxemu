// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/scope_exit.h"

#include "core/core.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {

AppletStorageChannel::AppletStorageChannel(KernelHelpers::ServiceContext& context)
    : m_event(context) {}
AppletStorageChannel::~AppletStorageChannel() = default;

void AppletStorageChannel::Push(std::shared_ptr<IStorage> storage) {
    std::scoped_lock lk{m_lock};

    m_data.emplace_back(std::move(storage));
    m_event.Signal();
}

Result AppletStorageChannel::Pop(std::shared_ptr<IStorage>* out_storage) {
    std::scoped_lock lk{m_lock};

    SCOPE_EXIT {
        if (m_data.empty()) {
            m_event.Clear();
        }
    };

    R_UNLESS(!m_data.empty(), AM::ResultNoDataInChannel);

    *out_storage = std::move(m_data.front());
    m_data.pop_front();

    R_SUCCEED();
}

Kernel::KReadableEvent* AppletStorageChannel::GetEvent() {
    return m_event.GetHandle();
}

AppletDataBroker::AppletDataBroker(Core::System& system_)
    : system(system_), context(system_, "AppletDataBroker"), in_data(context),
      interactive_in_data(context), out_data(context), interactive_out_data(context),
      state_changed_event(context), is_completed(false) {}

AppletDataBroker::~AppletDataBroker() = default;


void AppletDataBroker::SetCallerApplet(std::weak_ptr<Applet> caller, std::weak_ptr<Applet> child) {
    std::scoped_lock lk{lock};
    caller_applet = std::move(caller);
    child_applet = std::move(child);
}

void AppletDataBroker::SignalCompletion(bool notify_window_system) {
    std::weak_ptr<Applet> caller_applet_copy;
    std::weak_ptr<Applet> child_applet_copy;
    {
        std::scoped_lock lk{lock};

        if (is_completed) {
            return;
        }

        is_completed = true;
        state_changed_event.Signal();
        caller_applet_copy = caller_applet;
        child_applet_copy = child_applet;
    }

    const auto child = child_applet_copy.lock();

    if (auto caller = caller_applet_copy.lock(); caller) {
        std::scoped_lock caller_lk{caller->lock};
        if (child) {
            caller->child_applets.remove(child);
        }
    }

    if (notify_window_system) {
        if (child) {
            system.GetAppletManager().GetWindowSystem().NotifyAppletStopped(*child);
        } else {
            system.GetAppletManager().GetWindowSystem().NotifyAppletStateChanged();
        }
    }
}

} // namespace Service::AM
