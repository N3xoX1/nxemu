#pragma once
#include "state.h"
namespace Tegra::Decoder {
class H264 {
public:
    explicit H264(Host1x::Host1x&) {}
    auto ComposeFrame(const auto&, size_t* config, bool) {
        *config = MediaCodecTest::H264ConfigurationSize;
        return std::span<const u8>{MediaCodecTest::H264};
    }
    std::pair<u32,u32> GetFrameDimensions() const { return {64,32}; }
};
}
