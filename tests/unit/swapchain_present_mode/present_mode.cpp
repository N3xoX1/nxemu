// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "yuzu_video_core/renderer_vulkan/swapchain_present_mode.h"

int main() {
    unsigned failures{};
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    for (unsigned support = 0; support < 8; ++support) {
        const bool immediate = (support & 1) != 0;
        const bool mailbox = (support & 2) != 0;
        const bool relaxed = (support & 4) != 0;
        const auto mode = [&](VSyncMode setting) {
            return Vulkan::SelectPresentMode(setting, false, immediate, mailbox, relaxed);
        };
        const auto expected_uncapped = immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                       : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                                 : VK_PRESENT_MODE_FIFO_KHR;
        for (const VSyncMode setting : {VSyncMode::Fifo, VSyncMode::FifoRelaxed,
                                        VSyncMode::Mailbox, VSyncMode::Immediate}) {
            check(Vulkan::SelectPresentMode(setting, true, immediate, mailbox, relaxed) ==
                      expected_uncapped,
                  "uncapped ignores VSync and prefers Immediate, then Mailbox");
        }
        check(mode(VSyncMode::Fifo) == VK_PRESENT_MODE_FIFO_KHR,
              "On remains FIFO even when Mailbox and Immediate are available");
        check(mode(VSyncMode::Immediate) == (immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR),
              "Off uses Immediate only when supported");
        check(mode(VSyncMode::Mailbox) == (mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR),
              "lowest delay preserves Mailbox with a synchronized fallback");
        check(mode(VSyncMode::FifoRelaxed) == (relaxed ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR),
              "late-frame tearing requires an explicit supported relaxed mode");
    }
    return failures ? 1 : 0;
}
