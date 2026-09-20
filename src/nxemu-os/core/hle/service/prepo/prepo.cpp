// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/prepo/prepo.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/reporter.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"

namespace Service::PlayReport
{

class PlayReport final : public ServiceFramework<PlayReport>
{
public:
    explicit PlayReport(const char * name, Core::System & system_) :
        ServiceFramework{system_, name}
    {
        // clang-format off
        static const FunctionInfo functions[] = {
            {10100, &PlayReport::SaveReport<Core::Reporter::PlayReportType::Old>, "SaveReportOld"},
            {10101, &PlayReport::SaveReportWithUser<Core::Reporter::PlayReportType::Old>, "SaveReportWithUserOld"},
            {10102, &PlayReport::SaveReport<Core::Reporter::PlayReportType::Old2>, "SaveReportOld2"},
            {10103, &PlayReport::SaveReportWithUser<Core::Reporter::PlayReportType::Old2>, "SaveReportWithUserOld2"},
            {10104, &PlayReport::SaveReport<Core::Reporter::PlayReportType::Old3>, "SaveReportOld3"},
            {10105, &PlayReport::SaveReportWithUser<Core::Reporter::PlayReportType::Old3>, "SaveReportWithUserOld3"},
            {10106, &PlayReport::SaveReport<Core::Reporter::PlayReportType::New>, "SaveReport"},
            {10107, &PlayReport::SaveReportWithUser<Core::Reporter::PlayReportType::New>, "SaveReportWithUser"},
            {10200, &PlayReport::RequestImmediateTransmission, "RequestImmediateTransmission"},
            {10300, &PlayReport::GetTransmissionStatus, "GetTransmissionStatus"},
            {10400, &PlayReport::GetSystemSessionId, "GetSystemSessionId"},
            {20100, &PlayReport::SaveSystemReportOld, "SaveSystemReport"},
            {20101, &PlayReport::SaveSystemReportWithUserOld, "SaveSystemReportWithUser"},
            {20102, &PlayReport::SaveSystemReport, "SaveSystemReport"},
            {20103, &PlayReport::SaveSystemReportWithUser, "SaveSystemReportWithUser"},
            {20200, nullptr, "SetOperationMode"},
            {30100, nullptr, "ClearStorage"},
            {30200, nullptr, "ClearStatistics"},
            {30300, nullptr, "GetStorageUsage"},
            {30400, nullptr, "GetStatistics"},
            {30401, nullptr, "GetThroughputHistory"},
            {30500, nullptr, "GetLastUploadError"},
            {30600, nullptr, "GetApplicationUploadSummary"},
            {40100, &PlayReport::IsUserAgreementCheckEnabled, "IsUserAgreementCheckEnabled"},
            {40101, &PlayReport::SetUserAgreementCheckEnabled, "SetUserAgreementCheckEnabled"},
            {50100, nullptr, "ReadAllApplicationReportFiles"},
            {90100, nullptr, "ReadAllReportFiles"},
            {90101, nullptr, "Unknown90101"},
            {90102, nullptr, "Unknown90102"},
            {90200, nullptr, "GetStatistics"},
            {90201, nullptr, "GetThroughputHistory"},
            {90300, nullptr, "GetLastUploadError"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    static u64 GetClientProcessId(HLERequestContext& ctx, u64 raw_process_id) {
        const u64 client_process_id = ctx.GetPID();
        return client_process_id != 0 ? client_process_id : raw_process_id;
    }

    template <Core::Reporter::PlayReportType Type>
    void SaveReport(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto raw_process_id = rp.PopRaw<u64>();
        const auto process_id = GetClientProcessId(ctx, raw_process_id);

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, type={:02X}, process_id={:016X}, game_room_size={:016X}, "
                  "report_size={:016X}",
                  Type, process_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Type, system.GetApplicationProcessProgramID(), {game_room, report},
                                process_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    template <Core::Reporter::PlayReportType Type>
    void SaveReportWithUser(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto user_id = rp.PopRaw<u128>();
        const auto raw_process_id = rp.PopRaw<u64>();
        const auto process_id = GetClientProcessId(ctx, raw_process_id);

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, type={:02X}, user_id={:016X}{:016X}, process_id={:016X}, "
                  "game_room_size={:016X}, report_size={:016X}",
                  Type, user_id[1], user_id[0], process_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Type, system.GetApplicationProcessProgramID(), {game_room, report},
                                process_id, user_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RequestImmediateTransmission(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PREPO, "called");
        immediate_transmission_enabled = true;

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetTransmissionStatus(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PREPO, "called");

        const s32 status = immediate_transmission_enabled && user_agreement_check_enabled ? 1 : 0;

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(status);
    }

    void GetSystemSessionId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PREPO, "called");

        if (system_session_id == 0) {
            system_session_id =
                Kernel::Board::Nintendo::Nx::KSystemControl::GenerateRandomU64();
        }

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(system_session_id);
    }

    void SaveSystemReportOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto title_id = rp.PopRaw<u64>();

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, title_id={:016X}, game_room_size={:016X}, report_size={:016X}",
                  title_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Core::Reporter::PlayReportType::System, title_id,
                                {game_room, report});

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SaveSystemReportWithUserOld(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto user_id = rp.PopRaw<u128>();
        const auto title_id = rp.PopRaw<u64>();

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, user_id={:016X}{:016X}, title_id={:016X}, "
                  "game_room_size={:016X}, report_size={:016X}",
                  user_id[1], user_id[0], title_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Core::Reporter::PlayReportType::System, title_id,
                                {game_room, report}, std::nullopt, user_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // (21.0.0+) buffers: [0x9 (X), 0x5 (A)], inbytes: 0x10
    void SaveSystemReport(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto field0 = rp.PopRaw<u64>();
        const auto title_id = rp.PopRaw<u64>();

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, field0={:016X}, title_id={:016X}, game_room_size={:016X}, "
                  "report_size={:016X}",
                  field0, title_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Core::Reporter::PlayReportType::System, title_id,
                                {game_room, report});

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // (21.0.0+) buffers: [0x9 (X), 0x5 (A)], inbytes: 0x20
    void SaveSystemReportWithUser(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        // 21.0.0+: field0 (u64), user_id (u128), title_id (u64)
        const auto field0 = rp.PopRaw<u64>();
        const auto user_id = rp.PopRaw<u128>();
        const auto title_id = rp.PopRaw<u64>();

        const auto game_room = ctx.ReadBufferX(0);
        const auto report = ctx.ReadBufferA(0);

        LOG_DEBUG(Service_PREPO,
                  "called, field0={:016X}, user_id={:016X}{:016X}, title_id={:016X}, "
                  "game_room_size={:016X}, report_size={:016X}",
                  field0, user_id[1], user_id[0], title_id, game_room.size(), report.size());

        const auto& reporter{system.GetReporter()};
        reporter.SavePlayReport(Core::Reporter::PlayReportType::System, title_id,
                                {game_room, report}, std::nullopt, user_id);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsUserAgreementCheckEnabled(HLERequestContext& ctx) {
        LOG_DEBUG(Service_PREPO, "called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(user_agreement_check_enabled);
    }

    void SetUserAgreementCheckEnabled(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        user_agreement_check_enabled = rp.PopRaw<u8>() != 0;

        LOG_DEBUG(Service_PREPO, "called, enabled={}", user_agreement_check_enabled);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    bool immediate_transmission_enabled{};
    bool user_agreement_check_enabled{true};
    u64 system_session_id{};

};

void LoopProcess(Core::System & system)
{
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("prepo:a", std::make_shared<PlayReport>("prepo:a", system));
    server_manager->RegisterNamedService("prepo:a2", std::make_shared<PlayReport>("prepo:a2", system));
    server_manager->RegisterNamedService("prepo:m", std::make_shared<PlayReport>("prepo:m", system));
    server_manager->RegisterNamedService("prepo:s", std::make_shared<PlayReport>("prepo:s", system));
    server_manager->RegisterNamedService("prepo:u", std::make_shared<PlayReport>("prepo:u", system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::PlayReport
