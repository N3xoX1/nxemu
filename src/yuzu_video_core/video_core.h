// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <nxemu-module-spec/video.h>

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {
class Display;
}

namespace VideoCore {

class RendererBase;

/// Creates an emulated GPU instance using the given system context. `display` is what the Vulkan
/// renderer draws with and presents to; the other renderers do without.
std::unique_ptr<Tegra::GPU> CreateGPU(ISystemModules & modules, Core::Frontend::EmuWindow & emu_window, Tegra::Host1x::Host1x & host1x, Vulkan::Display * display);

struct DisplayDeleter {
    void operator()(Vulkan::Display * display) const;
};
using DisplayPtr = std::unique_ptr<Vulkan::Display, DisplayDeleter>;

/// Creates the device the Vulkan renderer draws with, presenting to the window. Returns null on
/// failure.
DisplayPtr CreateDisplay(Core::Frontend::EmuWindow & emu_window);

/// The calls IVideo offers a frontend that draws its interface on the display.
bool AttachInterface(Vulkan::Display & display, VideoInterfaceDevice & device);
bool BeginInterfaceFrame(Vulkan::Display & display, uint32_t width, uint32_t height, VideoInterfaceFrame & frame);
void EndInterfaceFrame(Vulkan::Display & display, const VideoInterfaceArea * game_area);
void WaitInterfaceIdle(Vulkan::Display & display);

} // namespace VideoCore
