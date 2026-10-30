#pragma once
#include "state.h"
namespace Tegra::Decoder {
class VP9 {
public:
    explicit VP9(Host1x::Host1x&) {
        ++MediaCodecTest::vp9_instances;
        MediaCodecTest::vp9_compositions = 0;
    }
    void ComposeFrame(const auto&) { ++MediaCodecTest::vp9_compositions; }
    bool IsKeyFrame() const { return true; }
    bool WasFrameHidden() const { return true; }
    std::pair<u32,u32> GetFrameDimensions() const { return {64,32}; }
    std::span<const u8> GetFrameBytes() const { return MediaCodecTest::VP9; }
};
}
