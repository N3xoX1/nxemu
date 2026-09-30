// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include "yuzu_common/logging/log.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/mm/mm_u.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm.h"

namespace Service::MM {

namespace {

struct Session {
    u32 module{};
    u32 request_id{};
    u32 requested_clock_rate{};
    bool has_active_request{};

    void SetAndWait(u32 clock_rate) {
        if (clock_rate == 0 || (clock_rate & 0x80000000U) != 0) {
            requested_clock_rate = 0;
            has_active_request = false;
            return;
        }

        requested_clock_rate = clock_rate;
        has_active_request = true;
    }
};

} // namespace

class MM_U final : public ServiceFramework<MM_U> {
public:
    explicit MM_U(Core::System& system_) : ServiceFramework{system_, "mm:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &MM_U::InitializeOld, "InitializeOld"},
            {1, &MM_U::FinalizeOld, "FinalizeOld"},
            {2, &MM_U::SetAndWaitOld, "SetAndWaitOld"},
            {3, &MM_U::GetOld, "GetOld"},
            {4, &MM_U::Initialize, "Initialize"},
            {5, &MM_U::Finalize, "Finalize"},
            {6, &MM_U::SetAndWait, "SetAndWait"},
            {7, &MM_U::Get, "Get"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    Session* FindSessionByModule(u32 module) {
        for (auto& session : sessions) {
            if (session.module == module) {
                return &session;
            }
        }
        return nullptr;
    }

    Session* FindSessionById(u32 request_id) {
        for (auto& session : sessions) {
            if (session.request_id == request_id) {
                return &session;
            }
        }
        return nullptr;
    }

    u32 RegisterSession(u32 module) {
        const u32 request_id = next_request_id++;
        sessions.push_back(Session{module, request_id});
        return request_id;
    }

    u32 GetEffectiveClockRate(u32 module) const {
        u32 clock_rate{};
        for (const auto& session : sessions) {
            if (session.module == module && session.has_active_request &&
                session.requested_clock_rate > clock_rate) {
                clock_rate = session.requested_clock_rate;
            }
        }
        return clock_rate;
    }

    void RemoveSessionByModule(u32 module) {
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if (it->module == module) {
                sessions.erase(it);
                return;
            }
        }
    }

    void RemoveSessionById(u32 request_id) {
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if (it->request_id == request_id) {
                sessions.erase(it);
                return;
            }
        }
    }

    void InitializeOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 module = rp.Pop<u32>();
        const u32 priority = rp.Pop<u32>();
        const bool is_auto_clear_event = rp.Pop<u32>() != 0;
        const u32 request_id = RegisterSession(module);

        LOG_DEBUG(Service_MM,
                  "called, module=0x{:X}, priority=0x{:X}, auto_clear={}, request_id=0x{:X}",
                  module, priority, is_auto_clear_event, request_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void FinalizeOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 module = rp.Pop<u32>();
        RemoveSessionByModule(module);

        LOG_DEBUG(Service_MM, "called, module=0x{:X}", module);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetAndWaitOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 module = rp.Pop<u32>();
        const u32 requested_clock_rate = rp.Pop<u32>();
        const s32 timeout = rp.Pop<s32>();

        if (auto* session = FindSessionByModule(module); session != nullptr) {
            session->SetAndWait(requested_clock_rate);
        }

        const u32 actual_clock_rate = GetEffectiveClockRate(module);
        LOG_TRACE(Service_MM,
                  "called, module=0x{:X}, requested=0x{:X}, actual={}, timeout={}", module,
                  requested_clock_rate, actual_clock_rate, timeout);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 module = rp.Pop<u32>();
        const u32 actual_clock_rate = GetEffectiveClockRate(module);

        LOG_TRACE(Service_MM, "called, module=0x{:X}, actual={}", module, actual_clock_rate);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(actual_clock_rate);
    }

    void Initialize(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 module = rp.Pop<u32>();
        const u32 priority = rp.Pop<u32>();
        const bool is_auto_clear_event = rp.Pop<u32>() != 0;
        const u32 request_id = RegisterSession(module);

        LOG_DEBUG(Service_MM,
                  "called, module=0x{:X}, priority=0x{:X}, auto_clear={}, request_id=0x{:X}",
                  module, priority, is_auto_clear_event, request_id);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(request_id);
    }

    void Finalize(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 request_id = rp.Pop<u32>();
        RemoveSessionById(request_id);

        LOG_DEBUG(Service_MM, "called, request_id=0x{:X}", request_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetAndWait(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 request_id = rp.Pop<u32>();
        const u32 requested_clock_rate = rp.Pop<u32>();
        const s32 timeout = rp.Pop<s32>();

        u32 actual_clock_rate{};
        u32 module{};
        bool found{};
        if (auto* session = FindSessionById(request_id); session != nullptr) {
            session->SetAndWait(requested_clock_rate);
            module = session->module;
            actual_clock_rate = GetEffectiveClockRate(module);
            found = true;
        }

        LOG_TRACE(Service_MM,
                  "called, request_id=0x{:X}, module=0x{:X}, requested=0x{:X}, actual={}, timeout={}, "
                  "found={}",
                  request_id, module, requested_clock_rate, actual_clock_rate, timeout, found);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Get(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 request_id = rp.Pop<u32>();

        u32 actual_clock_rate{};
        u32 module{};
        bool found{};
        if (const auto* session = FindSessionById(request_id); session != nullptr) {
            module = session->module;
            actual_clock_rate = GetEffectiveClockRate(module);
            found = true;
        }

        LOG_TRACE(Service_MM, "called, request_id=0x{:X}, module=0x{:X}, actual={}, found={}",
                  request_id, module, actual_clock_rate, found);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(actual_clock_rate);
    }

    std::vector<Session> sessions;
    u32 next_request_id{1};
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("mm:u", std::make_shared<MM_U>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::MM
