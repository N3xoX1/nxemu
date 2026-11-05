// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/present/upscaler.h"
#include "yuzu_video_core/renderer_vulkan/present/fsr.h"
#include <cstdlib>
#include <string_view>

namespace Vulkan {
namespace {
class FsrSpatial final : public Upscaler {
public:
    FsrSpatial(const Device& d, MemoryAllocator& a, size_t count, VkExtent2D out, bool easu)
        : fsr(d, a, count, out, easu), count(count), output(out) {}
    VideoCore::Upscaling::Backend Backend() const override { return VideoCore::Upscaling::Backend::Fsr1; }
    VideoCore::Upscaling::Capabilities Capabilities() const override { return {.alpha = true}; }
    std::optional<UpscaleResult> Draw(Scheduler& scheduler, const UpscaleFrame& frame) override {
        if (!IsFrameSupported(Capabilities(), frame) ||
            frame.image_index >= count || frame.parameters.output.width != output.width ||
            frame.parameters.output.height != output.height) return std::nullopt;
        return UpscaleResult{fsr.Draw(scheduler, frame.image_index, frame.color, frame.color_view,
            frame.color_extent, frame.crop)};
    }
private:
    FSR fsr;
    size_t count;
    VkExtent2D output;
};
}
#ifdef NXEMU_ENABLE_METALFX
std::unique_ptr<Upscaler> CreateMetalFxSpatial(const Device&, MemoryAllocator&, size_t, VkExtent2D);
#endif
std::unique_ptr<Upscaler> CreateSpatialUpscaler(VideoCore::Upscaling::Backend backend,
    const Device& device, MemoryAllocator& allocator, size_t count, VkExtent2D output, bool easu_only) {
    if (!count || !output.width || !output.height) return {};
    if (backend == VideoCore::Upscaling::Backend::Fsr1)
        return std::make_unique<FsrSpatial>(device, allocator, count, output, easu_only);
#ifdef NXEMU_ENABLE_METALFX
    if (backend == VideoCore::Upscaling::Backend::MetalFxSpatial)
        return CreateMetalFxSpatial(device, allocator, count, output);
#endif
    return {};
}
bool RequestMetalFxQuality() {
    static const bool requested = [] {
        const char* value = std::getenv("NXEMU_QUALITY_UPSCALER");
        return value && std::string_view(value) == "metalfx";
    }();
    return requested;
}
} // namespace Vulkan
