#pragma once
#include "state.h"
namespace Tegra::Decoder {
class VP8 {
public:
    explicit VP8(Host1x::Host1x&) {}
    std::span<const u8> ComposeFrame(const auto&) { return MediaCodecTest::H264; }
};
}
