// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>

#include "yuzu_common/polyfill_thread.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"

namespace Service::Sockets {

namespace {
using DeferralClock = std::chrono::steady_clock;

Kernel::KEvent* g_bsd_deferral_event{};
std::mutex g_bsd_deferral_mutex;
std::condition_variable g_bsd_deferral_cv;
std::map<const void*, DeferralClock::time_point> g_bsd_poll_deadlines;
u64 g_bsd_deadline_generation{};
} // namespace

bool IsBsdDeferralEnabled() {
    return g_bsd_deferral_event != nullptr;
}

void SignalBsdDeferral() {
    if (g_bsd_deferral_event) {
        g_bsd_deferral_event->Signal();
    }
}

void RegisterBsdDeferredPoll(
    const void* request_key, std::optional<std::chrono::steady_clock::time_point> deadline) {
    // poll(-1) is purely event-driven. Only finite timeouts need scheduler state.
    if (!deadline) {
        return;
    }

    {
        std::scoped_lock lock{g_bsd_deferral_mutex};
        g_bsd_poll_deadlines.insert_or_assign(request_key, *deadline);
        ++g_bsd_deadline_generation;
    }
    g_bsd_deferral_cv.notify_all();
}

void UnregisterBsdDeferredPoll(const void* request_key) {
    bool changed = false;
    {
        std::scoped_lock lock{g_bsd_deferral_mutex};
        changed = g_bsd_poll_deadlines.erase(request_key) != 0;
        if (changed) {
            ++g_bsd_deadline_generation;
        }
    }
    if (changed) {
        g_bsd_deferral_cv.notify_all();
    }
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("bsd:s", std::make_shared<BSD>(system, "bsd:s"));
    server_manager->RegisterNamedService("bsd:u", std::make_shared<BSD>(system, "bsd:u"));
    server_manager->RegisterNamedService("bsdcfg", std::make_shared<BSDCFG>(system));
    server_manager->RegisterNamedService("nsd:a", std::make_shared<NSD>(system, "nsd:a"));
    server_manager->RegisterNamedService("nsd:u", std::make_shared<NSD>(system, "nsd:u"));
    server_manager->RegisterNamedService("sfdnsres", std::make_shared<SFDNSRES>(system));

    Kernel::KEvent* deferral_event{};
    server_manager->ManageDeferral(&deferral_event);
    ASSERT(deferral_event != nullptr);
    g_bsd_deferral_event = deferral_event;

    server_manager->StartAdditionalHostThreads("bsdsocket", 2);

    {
        // EventFd state changes wake deferred requests directly. This scheduler is only for
        // finite poll timeouts; poll(-1) never registers a timer and remains purely event-driven.
        std::jthread deferral_scheduler([deferral_event](std::stop_token stop_token) {
            std::stop_callback stop_callback{stop_token, [] { g_bsd_deferral_cv.notify_all(); }};
            std::unique_lock lock{g_bsd_deferral_mutex};

            while (!stop_token.stop_requested()) {
                g_bsd_deferral_cv.wait(lock, [&] {
                    return stop_token.stop_requested() || !g_bsd_poll_deadlines.empty();
                });
                if (stop_token.stop_requested()) {
                    break;
                }

                const auto next_wake = std::min_element(
                    g_bsd_poll_deadlines.begin(), g_bsd_poll_deadlines.end(),
                    [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; })
                                           ->second;
                const u64 generation = g_bsd_deadline_generation;
                if (g_bsd_deferral_cv.wait_until(lock, next_wake, [&] {
                        return stop_token.stop_requested() ||
                               generation != g_bsd_deadline_generation;
                    })) {
                    continue;
                }

                const auto now = DeferralClock::now();
                bool expired = false;
                for (auto it = g_bsd_poll_deadlines.begin(); it != g_bsd_poll_deadlines.end();) {
                    if (it->second <= now) {
                        it = g_bsd_poll_deadlines.erase(it);
                        expired = true;
                    } else {
                        ++it;
                    }
                }
                if (expired) {
                    ++g_bsd_deadline_generation;
                    lock.unlock();
                    deferral_event->Signal();
                    lock.lock();
                }
            }
        });

        ServerManager::RunServer(std::move(server_manager));
    }

    g_bsd_deferral_event = nullptr;
    {
        std::scoped_lock lock{g_bsd_deferral_mutex};
        g_bsd_poll_deadlines.clear();
        ++g_bsd_deadline_generation;
    }
    g_bsd_deferral_cv.notify_all();
    deferral_event->Close();
}

} // namespace Service::Sockets
