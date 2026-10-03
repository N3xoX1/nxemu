// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <boost/container/flat_map.hpp>

#include "core/hle/service/nvnflinger/buffer_item.h"
#include "core/hle/service/nvnflinger/display.h"

namespace Service::Nvidia::Devices {
class nvdisp_disp0;
}

namespace Service::Nvnflinger {

using ConsumerId = s32;

class HardwareComposer {
public:
    explicit HardwareComposer();
    ~HardwareComposer();

    u32 ComposeLocked(f32* out_speed_scale, Display& display,
                      Nvidia::Devices::nvdisp_disp0& nvdisp);
    void RemoveLayerLocked(Display& display, ConsumerId consumer_id,
                           Nvidia::Devices::nvdisp_disp0& nvdisp);

private:
    u64 m_frame_number{0};

private:
    using ReleaseFrameNumber = u64;

    struct Framebuffer {
        android::BufferItem item{};
        ReleaseFrameNumber release_frame_number{};
        bool is_acquired{false};
    };

    enum class CacheStatus : u32 {
        NoBufferAvailable,
        BufferAcquired,
        CachedBufferReused,
    };

    boost::container::flat_map<ConsumerId, Framebuffer> m_framebuffers{};

private:
    bool TryAcquireFramebufferLocked(Layer& layer, Framebuffer& framebuffer);
    CacheStatus CacheFramebufferLocked(Layer& layer, ConsumerId consumer_id);
    void ReleaseFramebuffersLocked(Display& display);
};

} // namespace Service::Nvnflinger
