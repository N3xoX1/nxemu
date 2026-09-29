// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/process_holder.h"

namespace Service::AM {

ProcessHolder::ProcessHolder(std::shared_ptr<Applet> applet, Kernel::KProcess& process)
    : MultiWaitHolder(&process), m_applet(std::move(applet)), m_process(process) {
    m_process.Open();
}

ProcessHolder::~ProcessHolder() {
    m_process.Close();
}

} // namespace Service::AM
