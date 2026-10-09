// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/service.h"

namespace Service::NS {

class IReadOnlyApplicationControlDataInterface final
    : public ServiceFramework<IReadOnlyApplicationControlDataInterface> {
public:
    explicit IReadOnlyApplicationControlDataInterface(Core::System& system_);
    ~IReadOnlyApplicationControlDataInterface() override;
    void GetApplicationControlDataRequest(HLERequestContext& ctx);
    void GetApplicationControlData3Request(HLERequestContext& ctx);

public:
    Result GetApplicationControlData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     Out<u32> out_actual_size,
                                     ApplicationControlSource application_control_source,
                                     u64 application_id);
    Result GetApplicationDesiredLanguage(Out<ApplicationLanguage> out_desired_language,
                                         u32 supported_languages);
    Result ConvertApplicationLanguageToLanguageCode(Out<u64> out_language_code,
                                                    ApplicationLanguage application_language);
    Result GetApplicationControlData3(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     Out<u32> out_flags_a, Out<u32> out_flags_b,
                                     Out<u32> out_actual_size,
                                     ApplicationControlSource application_control_source,
                                     u8 resize_icon, u8 control_data_index, u64 application_id);
private:
    void ReplyApplicationControlData(HLERequestContext& ctx, bool modern);
};

} // namespace Service::NS
