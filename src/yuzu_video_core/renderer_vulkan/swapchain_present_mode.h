// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vulkan/vulkan_core.h>
#include "nxemu-video/video_settings.h"

namespace Vulkan {

// The speed limiter controls emulation speed. Presentation follows the selected
// VSync mode even when that limiter is disabled; unsupported modes fall back to FIFO.
constexpr VkPresentModeKHR SelectPresentMode(VSyncMode setting, bool immediate,
                                             bool mailbox, bool fifo_relaxed) {
    switch (setting) {
    case VSyncMode::Immediate:
        return immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    case VSyncMode::Mailbox:
        return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    case VSyncMode::FifoRelaxed:
        return fifo_relaxed ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
    case VSyncMode::Fifo:
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

} // namespace Vulkan
