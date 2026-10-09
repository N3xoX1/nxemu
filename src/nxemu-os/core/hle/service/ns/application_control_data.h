// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <nxemu-module-spec/application_control_data.h>

namespace Service::NS {

struct ControlDataResponse {
    uint32_t flags_a{};
    uint32_t flags_b{};
    uint32_t actual_size{};
};

// Only the installed application's primary control-data entry is available.
constexpr bool IsControlDataRequestSupported(uint8_t source, uint8_t resize, uint8_t index) {
    return source <= 2 && resize <= 1 && index == 0;
}

LoaderResultStatus WriteApplicationControlData(std::span<const uint8_t> data,
    std::span<uint8_t> output, bool modern, bool resize_icon, ControlDataResponse& response);

} // namespace Service::NS
