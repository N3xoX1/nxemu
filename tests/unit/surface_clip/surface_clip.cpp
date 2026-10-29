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
    v = Vulkan::SurfaceClipViewport(0, 0, 0, 0, false, 2.f);
    s = Vulkan::SurfaceClipScissor(0, 0, 0, 0, 2, 0);
    check(v.width == 1 && v.height == 1 && s.extent.width == 1 && s.extent.height == 1,
          "zero extents preserve Vulkan's nonzero viewport requirement");
    std::printf("Surface clip scaling: %u failures\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
