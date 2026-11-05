// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "yuzu_video_core/fsr_diagnostics.h"
#include "yuzu_video_core/renderer_opengl/gl_resource_manager.h"
#include <array>
#include <limits>

namespace OpenGL {

class PresentTimer {
  public:
    PresentTimer() {
        for (auto &slot : slots) {
            slot.start.Create(GL_TIMESTAMP);
            slot.end.Create(GL_TIMESTAMP);
        }
    }
    ~PresentTimer() {
        glFinish();
        Collect();
    }
    void Begin() {
        Collect();
        active = slots.size();
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].pending) {
                active = i;
                glQueryCounter(slots[i].start.handle, GL_TIMESTAMP);
                break;
            }
        }
    }
    void End(VideoCore::PresentDiagnostics::Sample sample) {
        const auto now = VideoCore::PresentDiagnostics::Clock::now();
        sample.frame_interval_ms = previous_frame.time_since_epoch().count() == 0 ? 0.0 :
            std::chrono::duration<double, std::milli>(now - previous_frame).count();
        previous_frame = now;
        if (active == slots.size()) {
            VideoCore::PresentDiagnostics::Write("OpenGL", sample,
                                                 std::numeric_limits<double>::quiet_NaN());
            return;
        }
        auto &slot = slots[active];
        glQueryCounter(slot.end.handle, GL_TIMESTAMP);
        slot.sample = sample;
        slot.pending = true;
    }

  private:
    void Collect() {
        for (auto &slot : slots) {
            if (!slot.pending)
                continue;
            GLint available{};
            glGetQueryObjectiv(slot.end.handle, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
                continue;
            GLuint64 start{}, end{};
            glGetQueryObjectui64v(slot.start.handle, GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v(slot.end.handle, GL_QUERY_RESULT, &end);
            VideoCore::PresentDiagnostics::Write("OpenGL", slot.sample,
                                                 static_cast<double>(end - start) / 1.0e6);
            slot.pending = false;
        }
    }
    struct Slot {
        OGLQuery start, end;
        VideoCore::PresentDiagnostics::Sample sample;
        bool pending{};
    };
    std::array<Slot, 16> slots;
    size_t active{slots.size()};
    VideoCore::PresentDiagnostics::Clock::time_point previous_frame;
};

} // namespace OpenGL
