// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/os/multi_wait_holder.h"
#include "yuzu_common/intrusive_list.h"

namespace Kernel {
class KProcess;
}

namespace Service::AM {

struct Applet;

class ProcessHolder : public MultiWaitHolder, public Common::IntrusiveListBaseNode<ProcessHolder> {
public:
    ProcessHolder(std::shared_ptr<Applet> applet, Kernel::KProcess& process);
    ~ProcessHolder();

    std::shared_ptr<Applet> GetApplet() const {
        return m_applet;
    }

    Kernel::KProcess& GetProcess() const {
        return m_process;
    }

private:
    std::shared_ptr<Applet> m_applet;
    Kernel::KProcess& m_process;
};

} // namespace Service::AM
