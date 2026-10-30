// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "yuzu_video_core/host1x/ffmpeg/ffmpeg.h"
#include "video_settings.h"
#include "fixtures.h"

VideoSettings videoSettings{};
int main() {
    using Codec = Tegra::Host1x::NvdecCommon::VideoCodec;
    videoSettings.nvdec_emulation = NvdecEmulation::Cpu;
    FFmpeg::DecodeApi api;
    std::queue<std::unique_ptr<FFmpeg::Frame>> frames;
    struct Input { Codec codec; std::span<const u8> packet; size_t configuration; };
    const Input inputs[] = {
        {Codec::H264,MediaCodecTest::H264,MediaCodecTest::H264ConfigurationSize},
        {Codec::VP8,MediaCodecTest::VP8,0},
        {Codec::VP9,MediaCodecTest::VP9,0},
        {Codec::H264,MediaCodecTest::H264,MediaCodecTest::H264ConfigurationSize},
    };
    for (const auto& input : inputs) {
        assert(api.Initialize(input.codec));
        assert(api.SendPacket(input.packet,input.configuration,{64,32,true},frames));
        assert(api.ReceiveFrames(frames) && frames.size()==1);
        assert(frames.front()->GetWidth()==64 && frames.front()->GetHeight()==32);
        frames.pop();
    }
    api.Reset();
    assert(!api.SendPacket(MediaCodecTest::H264,0,{},frames));
}
