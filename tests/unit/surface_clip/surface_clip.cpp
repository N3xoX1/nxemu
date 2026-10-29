// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include "yuzu_video_core/renderer_vulkan/surface_clip_state.h"
int main() {
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    for (u32 factor : {1U, 2U, 3U}) {
        for (bool lower : {false, true}) {
            auto v = Vulkan::SurfaceClipViewport(8, 12, 128, 192, lower, float(factor));
            auto s = Vulkan::SurfaceClipScissor(8, 12, 128, 192, factor, 0);
            check(v.x == 8 * factor && v.width == 128 * factor, "scaled pixel-space X viewport");
            const float top = lower ? v.y + v.height : v.y;
            const float bottom = lower ? v.y : v.y + v.height;
            check(top == 12 * factor && bottom == 204 * factor, "scaled viewport Y bounds and origin");
            check(s.offset.x == s32(v.x) && s.offset.y == s32(top) &&
                  s.extent.width == u32(v.width) && s.extent.height == u32(bottom - top),
                  "scissor covers the complete scaled portrait target");
            // PositionPass maps a logical pixel to NDC; host viewport must map it
            // back to its scaled target pixel, for both origins and every scale.
            const float ndc_x = 2.f * 64.f / 128.f - 1.f;
            check(v.x + (ndc_x + 1.f) * v.width * .5f == (8 + 64) * factor,
                  "logical portrait center lands at scaled pixel center");
            check(v.minDepth == 0 && v.maxDepth == 1, "depth range preserved");
        }
    }
    auto v = Vulkan::SurfaceClipViewport(8, 12, 128, 192, false, .5f);
    auto s = Vulkan::SurfaceClipScissor(8, 12, 128, 192, 1, 1);
    check(v.x == 4 && v.y == 6 && v.width == 64 && v.height == 96 &&
          s.extent.width == 64 && s.extent.height == 96, "downscale stays consistent");
    // Fractional scales: the scissor must contain the exact scaled rectangle and stay
    // within one pixel of it. 1.5x is 3 >> 1, 0.75x is 3 >> 2 and 0.5x is 1 >> 1.
    struct Scale { u32 up; u32 shift; };
    for (const Scale scale : {Scale{3, 1}, Scale{3, 2}, Scale{1, 1}}) {
        const float factor = float(scale.up) / float(1U << scale.shift);
        for (u32 x : {0U, 1U, 3U, 7U}) {
            for (u32 width : {1U, 3U, 5U, 129U}) {
                const u32 y = x + 1;
                const u32 height = width + 2;
                s = Vulkan::SurfaceClipScissor(x, y, width, height, scale.up, scale.shift);
                const float left = float(x) * factor;
                const float right = float(x + width) * factor;
                const float top = float(y) * factor;
                const float bottom = float(y + height) * factor;
                const float s_right = float(s.offset.x) + float(s.extent.width);
                const float s_bottom = float(s.offset.y) + float(s.extent.height);
                check(float(s.offset.x) <= left && s_right >= right &&
                      float(s.offset.y) <= top && s_bottom >= bottom,
                      "fractional scale scissor covers the scaled rectangle");
                check(float(s.offset.x) > left - 1.f && s_right < right + 1.f &&
                      float(s.offset.y) > top - 1.f && s_bottom < bottom + 1.f,
                      "fractional scale scissor stays within one pixel");
            }
        }
    }
    s = Vulkan::SurfaceClipScissor(1, 0, 3, 4, 3, 1);
    check(s.offset.x == 1 && s.extent.width == 5, "1.5x keeps the last partial pixel");

    v = Vulkan::SurfaceClipViewport(0, 0, 0, 0, false, 2.f);
    s = Vulkan::SurfaceClipScissor(0, 0, 0, 0, 2, 0);
    check(v.width == 1 && v.height == 1 && s.extent.width == 1 && s.extent.height == 1,
          "zero extents preserve Vulkan's nonzero viewport requirement");
    std::printf("Surface clip scaling: %u failures\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
