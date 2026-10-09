// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/ns/read_only_application_control_data_interface.h"
#include <vector>
#include <algorithm>
#include <cstddef>
#include "core/core.h"
#include "core/hle/service/ns/application_control_data.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/ns/ns_results.h"
#include "core/hle/service/set/settings_server.h"
#include "os_settings.h"

namespace Service::NS
{

namespace {
Result ReadControlData(Core::System& system, u64 application_id, std::vector<u8>& data) {
    data.resize(ApplicationControlData::MaxSize);
    u32 actual_size{};
    const auto status = system.GetSystemloader().GetPMControlData(application_id, data.data(),
        static_cast<u32>(data.size()), actual_size);
    if (status != LoaderResultStatus::Success || actual_size < ApplicationControlData::NacpSize ||
        actual_size > data.size()) {
        LOG_ERROR(Service_NS, "Control-data read failed: application_id={:016X}, status={}, size={:#x}",
                  application_id, static_cast<u32>(status), actual_size);
        R_THROW(ResultUnknown);
    }
    data.resize(actual_size);
    R_SUCCEED();
}
} // namespace

IReadOnlyApplicationControlDataInterface::IReadOnlyApplicationControlDataInterface(Core::System & system_) :
    ServiceFramework{system_, "IReadOnlyApplicationControlDataInterface"}
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IReadOnlyApplicationControlDataInterface::GetApplicationControlDataRequest, "GetApplicationControlData"},
        {1, D<&IReadOnlyApplicationControlDataInterface::GetApplicationDesiredLanguage>, "GetApplicationDesiredLanguage"},
        {2, D<&IReadOnlyApplicationControlDataInterface::ConvertApplicationLanguageToLanguageCode>, "ConvertApplicationLanguageToLanguageCode"},
        {3, nullptr, "ConvertLanguageCodeToApplicationLanguage"},
        {4, nullptr, "SelectApplicationDesiredLanguage"},
        {19, &IReadOnlyApplicationControlDataInterface::GetApplicationControlData3Request, "GetApplicationControlData3"},
        {23, &IReadOnlyApplicationControlDataInterface::GetApplicationControlData3Request, "GetApplicationControlData3"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IReadOnlyApplicationControlDataInterface::~IReadOnlyApplicationControlDataInterface() = default;

void IReadOnlyApplicationControlDataInterface::GetApplicationControlDataRequest(HLERequestContext& ctx) {
    ReplyApplicationControlData(ctx, false);
}

void IReadOnlyApplicationControlDataInterface::GetApplicationControlData3Request(HLERequestContext& ctx) {
    ReplyApplicationControlData(ctx, true);
}

void IReadOnlyApplicationControlDataInterface::ReplyApplicationControlData(HLERequestContext& ctx, bool modern) {
    struct Input {
        ApplicationControlSource source;
        u8 resize_icon;
        u8 index;
        u8 padding[5];
        u64 application_id;
    };
    static_assert(sizeof(Input) == 16 && offsetof(Input, application_id) == 8);
    // The two flag bytes are padding in the legacy command and must be ignored there.
    IPC::RequestParser parser{ctx};
    const auto input = parser.PopRaw<Input>();
    std::vector<u8> buffer(std::min(ctx.GetWriteBufferSize(), size_t{ApplicationControlData::MaxSize}));
    ControlDataResponse response;
    const auto output = OutBuffer<BufferAttr_HipcMapAlias>{std::span<u8>{buffer}};
    const Result result = modern
        ? GetApplicationControlData3(output, &response.flags_a, &response.flags_b,
            &response.actual_size, input.source, input.resize_icon, input.index, input.application_id)
        : GetApplicationControlData(output, &response.actual_size, input.source, input.application_id);
    if (result.IsFailure()) {
        // The generic CMIF wrapper writes its temporary output buffers even on failure.
        // Send an error-only reply here, without copying any bytes to guest memory.
        IPC::ResponseBuilder reply{ctx, 2};
        reply.Push(result);
        return;
    }
    ctx.WriteBuffer(buffer);
    IPC::ResponseBuilder reply{ctx, modern ? 5u : 3u};
    reply.Push(result);
    if (modern) {
        reply.Push(response.flags_a);
        reply.Push(response.flags_b);
    }
    reply.Push(response.actual_size);
}

Result IReadOnlyApplicationControlDataInterface::GetApplicationControlData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer, Out<u32> out_actual_size, ApplicationControlSource application_control_source, u64 application_id)
{
    LOG_INFO(Service_NS, "called with control_source={}, application_id={:016X}", application_control_source, application_id);

    *out_actual_size = 0;
    if (!IsControlDataRequestSupported(static_cast<u8>(application_control_source), 0, 0)) {
        R_THROW(ResultUnknown);
    }
    std::vector<u8> data;
    R_TRY(ReadControlData(system, application_id, data));
    ControlDataResponse response;
    if (WriteApplicationControlData(data, out_buffer, false, false, response) != LoaderResultStatus::Success) {
        R_THROW(ResultUnknown);
    }
    *out_actual_size = response.actual_size;
    R_SUCCEED();
}

Result IReadOnlyApplicationControlDataInterface::GetApplicationControlData3(
    OutBuffer<BufferAttr_HipcMapAlias> out_buffer, Out<u32> out_flags_a, Out<u32> out_flags_b,
    Out<u32> out_actual_size, ApplicationControlSource application_control_source,
    u8 resize_icon, u8 control_data_index, u64 application_id)
{
    *out_flags_a = 0;
    *out_flags_b = 0;
    *out_actual_size = 0;
    LOG_INFO(Service_NS, "called with control_source={}, resize={}, index={}, application_id={:016X}",
             application_control_source, resize_icon, control_data_index, application_id);
    if (!IsControlDataRequestSupported(static_cast<u8>(application_control_source), resize_icon, control_data_index)) {
        R_THROW(ResultUnknown);
    }
    std::vector<u8> data;
    R_TRY(ReadControlData(system, application_id, data));
    ControlDataResponse response;
    const auto status = WriteApplicationControlData(data, out_buffer, true, resize_icon == 1, response);
    if (status != LoaderResultStatus::Success) {
        LOG_ERROR(Service_NS, "Control-data response failed: application_id={:016X}, status={}, buffer={:#x}",
                  application_id, static_cast<u32>(status), out_buffer.size());
        R_THROW(ResultUnknown);
    }
    *out_flags_a = response.flags_a;
    *out_flags_b = response.flags_b;
    *out_actual_size = response.actual_size;
    R_SUCCEED();
}

Result IReadOnlyApplicationControlDataInterface::GetApplicationDesiredLanguage(
    Out<ApplicationLanguage> out_desired_language, u32 supported_languages)
{
    LOG_INFO(Service_NS, "called with supported_languages={:08X}", supported_languages);

    // Get language code from settings
    const auto language_code = Set::GetLanguageCodeFromIndex(static_cast<s32>(osSettings.language_index));

    // Convert to application language, get priority list
    const auto application_language = ConvertToApplicationLanguage(language_code);
    if (application_language == std::nullopt)
    {
        LOG_ERROR(Service_NS, "Could not convert application language! language_code={}", language_code);
        R_THROW(Service::NS::ResultApplicationLanguageNotFound);
    }
    const auto priority_list = GetApplicationLanguagePriorityList(*application_language);
    if (!priority_list)
    {
        LOG_ERROR(Service_NS, "Could not find application language priorities! application_language={}", *application_language);
        R_THROW(Service::NS::ResultApplicationLanguageNotFound);
    }

    // Try to find a valid language.
    for (const auto lang : *priority_list)
    {
        const auto supported_flag = GetSupportedLanguageFlag(lang);
        if (supported_languages == 0 || (supported_languages & supported_flag) == supported_flag)
        {
            *out_desired_language = lang;
            R_SUCCEED();
        }
    }

    LOG_ERROR(Service_NS, "Could not find a valid language! supported_languages={:08X}", supported_languages);
    R_THROW(Service::NS::ResultApplicationLanguageNotFound);
}

Result IReadOnlyApplicationControlDataInterface::ConvertApplicationLanguageToLanguageCode(Out<u64> out_language_code, ApplicationLanguage application_language)
{
    const auto language_code = ConvertToLanguageCode(application_language);
    if (language_code == std::nullopt)
    {
        LOG_ERROR(Service_NS, "Language not found! application_language={}", application_language);
        R_THROW(Service::NS::ResultApplicationLanguageNotFound);
    }

    *out_language_code = static_cast<u64>(*language_code);
    R_SUCCEED();
}

} // namespace Service::NS
