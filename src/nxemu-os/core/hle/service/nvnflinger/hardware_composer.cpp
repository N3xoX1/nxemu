// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <boost/container/small_vector.hpp>

#include "yuzu_common/logging/log.h"

#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvnflinger/buffer_item.h"
#include "core/hle/service/nvnflinger/buffer_item_consumer.h"
#include "core/hle/service/nvnflinger/hardware_composer.h"
#include "core/hle/service/nvnflinger/hwc_layer.h"
#include "core/hle/service/nvnflinger/ui/graphic_buffer.h"

namespace Service::Nvnflinger {

namespace {

s32 NormalizeSwapInterval(f32* out_speed_scale, s32 swap_interval) {
    if (swap_interval <= 0) {
        // As an extension, treat nonpositive swap interval as speed multiplier.
        if (out_speed_scale) {
            *out_speed_scale = 2.f * static_cast<f32>(1 - swap_interval);
        }

        swap_interval = 1;
    }

    if (swap_interval >= 5) {
        // As an extension, treat high swap interval as precise speed control.
        if (out_speed_scale) {
            *out_speed_scale = static_cast<f32>(swap_interval) / 100.f;
        }

        swap_interval = 1;
    }

    return swap_interval;
}

} // namespace

HardwareComposer::HardwareComposer() = default;
HardwareComposer::~HardwareComposer() = default;

u32 HardwareComposer::ComposeLocked(f32* out_speed_scale, Display& display,
                                    Nvidia::Devices::nvdisp_disp0& nvdisp,
                                    bool allow_framebuffer_update) {
    boost::container::small_vector<HwcLayer, 2> composition_stack;

    // Set default speed limit to 100%.
    *out_speed_scale = 1.0f;

    // Determine the number of vsync periods to wait before composing again.
    std::optional<s32> swap_interval{};
    bool has_pending_frame{};
    bool missing_presented_buffer{};

    // Acquire all necessary framebuffers.
    for (auto& layer : display.stack.layers) {
        const auto consumer_id = layer->consumer_id;
        const auto result =
            this->CacheFramebufferLocked(*layer, consumer_id, allow_framebuffer_update);

        if (result == CacheStatus::NoBufferAvailable) {
            const auto it = m_framebuffers.find(consumer_id);
            if (layer->visible && it != m_framebuffers.end() &&
                it->second.waiting_for_replacement) {
                missing_presented_buffer = true;
            }
            continue;
        }

        const auto& buffer = m_framebuffers[consumer_id];
        has_pending_frame |= buffer.needs_present;
        const auto& item = buffer.item;
        if (allow_framebuffer_update && layer->visible) {
            const auto& igbp_buffer = *item.graphic_buffer;
            composition_stack.emplace_back(HwcLayer{
                .buffer_handle = igbp_buffer.BufferId(),
                .offset = igbp_buffer.Offset(),
                .format = igbp_buffer.ExternalFormat(),
                .width = igbp_buffer.Width(),
                .height = igbp_buffer.Height(),
                .stride = igbp_buffer.Stride(),
                .z_index = layer->z_index,
                .blending = layer->blending,
                .transform = static_cast<android::BufferTransformFlags>(item.transform),
                .crop_rect = item.crop,
                .acquire_fence = item.fence,
            });
        }

        const s32 item_swap_interval = NormalizeSwapInterval(out_speed_scale, item.swap_interval);
        if (swap_interval) {
            swap_interval = std::min(*swap_interval, item_swap_interval);
        } else {
            swap_interval = item_swap_interval;
        }
    }

    // A previously presented layer may temporarily have no buffer after releasing a single-slot
    // queue. Keep the last complete host frame until its replacement is available.
    if (allow_framebuffer_update && !missing_presented_buffer && has_pending_frame) {
        std::stable_sort(composition_stack.begin(), composition_stack.end(),
                         [&](auto& l, auto& r) { return l.z_index < r.z_index; });
        nvdisp.Composite(composition_stack);
        for (const auto& layer : display.stack.layers) {
            if (const auto it = m_framebuffers.find(layer->consumer_id);
                it != m_framebuffers.end()) {
                it->second.needs_present = false;
            }
        }
    }

    const u32 frame_advance = missing_presented_buffer ? 1 : swap_interval.value_or(1);
    m_frame_number += frame_advance;
    return frame_advance;
}

bool HardwareComposer::NeedsFramebufferUpdate(Display& display) const {
    for (const auto& layer : display.stack.layers) {
        const auto it = m_framebuffers.find(layer->consumer_id);
        if (it != m_framebuffers.end() && it->second.needs_present) {
            return true;
        }
        const auto [has_pending_buffer, producer_waiting] =
            layer->buffer_item_consumer->GetBufferQueueHints();

        if (it == m_framebuffers.end() || !it->second.is_acquired) {
            if (has_pending_buffer) {
                return true;
            }
            continue;
        }

        if (it->second.release_frame_number > m_frame_number) {
            continue;
        }

        if (has_pending_buffer || producer_waiting) {
            return true;
        }
    }

    return false;
}

void HardwareComposer::RemoveLayerLocked(Display& display, ConsumerId consumer_id,
                                         Nvidia::Devices::nvdisp_disp0& nvdisp) {
    const auto layer = display.stack.FindLayer(consumer_id);
    if (!layer) {
        return;
    }

    const auto it = m_framebuffers.find(consumer_id);
    if (it == m_framebuffers.end()) {
        return;
    }

    if (it->second.is_acquired) {
        nvdisp.CancelPendingComposite();
        layer->buffer_item_consumer->ReleaseBuffer(it->second.item, android::Fence::NoFence());
    }

    m_framebuffers.erase(it);
}

bool HardwareComposer::TryAcquireFramebufferLocked(Layer& layer, Framebuffer& framebuffer) {
    const auto status = layer.buffer_item_consumer->AcquireBuffer(&framebuffer.item, {}, false);
    if (status != android::Status::NoError) {
        return false;
    }

    static const bool diagnose = [] {
        const char* value = std::getenv("NXEMU_GPU_BINDING_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    if (diagnose) {
        const auto& item = framebuffer.item;
        LOG_INFO(Service_Nvnflinger,
                 "Present acquire: frame={}, slot={}, timestamp={}, automatic={}, droppable={}, "
                 "swap_interval={}, compositor_frame={}",
                 item.frame_number, item.slot, item.timestamp, item.is_auto_timestamp,
                 item.is_droppable, item.swap_interval, m_frame_number);
    }

    const s32 swap_interval = NormalizeSwapInterval(nullptr, framebuffer.item.swap_interval);
    framebuffer.release_frame_number = m_frame_number + swap_interval;
    framebuffer.is_acquired = true;
    framebuffer.waiting_for_replacement = false;
    framebuffer.needs_present = true;
    return true;
}

HardwareComposer::CacheStatus HardwareComposer::CacheFramebufferLocked(
    Layer& layer, ConsumerId consumer_id, bool allow_framebuffer_update) {
    const auto it = m_framebuffers.find(consumer_id);
    if (it != m_framebuffers.end()) {
        auto& framebuffer = it->second;

        if (framebuffer.is_acquired) {
            if (!allow_framebuffer_update || framebuffer.release_frame_number > m_frame_number) {
                return CacheStatus::CachedBufferReused;
            }

            const auto [pending_hint, waiting_hint] =
                layer.buffer_item_consumer->GetBufferQueueHints();
            if (!pending_hint && !waiting_hint) {
                return CacheStatus::CachedBufferReused;
            }

            bool has_pending_buffer{};
            const auto status = layer.buffer_item_consumer->ReleaseBufferIfNeeded(
                framebuffer.item, has_pending_buffer);
            if (status == android::Status::WouldBlock) {
                return CacheStatus::CachedBufferReused;
            }
            framebuffer.is_acquired = false;
            framebuffer.waiting_for_replacement = true;

            if (!has_pending_buffer) {
                return CacheStatus::NoBufferAvailable;
            }
        } else if (!allow_framebuffer_update) {
            return CacheStatus::NoBufferAvailable;
        }

        if (this->TryAcquireFramebufferLocked(layer, framebuffer)) {
            return CacheStatus::BufferAcquired;
        }

        return CacheStatus::NoBufferAvailable;
    }

    if (!allow_framebuffer_update) {
        return CacheStatus::NoBufferAvailable;
    }

    Framebuffer framebuffer{};
    if (this->TryAcquireFramebufferLocked(layer, framebuffer)) {
        m_framebuffers.emplace(consumer_id, std::move(framebuffer));
        return CacheStatus::BufferAcquired;
    }

    return CacheStatus::NoBufferAvailable;
}

} // namespace Service::Nvnflinger
