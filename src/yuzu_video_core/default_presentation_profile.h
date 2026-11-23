// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include "nxemu-module-spec/presentation_profile.h"

namespace VideoCore {

[[nodiscard]] ScalingFilter DetectDefaultPresentationProfile(int32_t vulkan_device_index);

} // namespace VideoCore
