// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/host1x/codecs/codec.h"
#include "yuzu_video_core/host1x/codecs/h264.h"
#include "yuzu_video_core/host1x/codecs/vp8.h"
#include "yuzu_video_core/host1x/codecs/vp9.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/memory_manager.h"

namespace Tegra {

Codec::Codec(Host1x::Host1x& host1x_, const Host1x::NvdecCommon::NvdecRegisters& regs) : 
    host1x(host1x_),
    state{regs},
    h264_decoder(std::make_unique<Decoder::H264>(host1x)),
    vp8_decoder(std::make_unique<Decoder::VP8>(host1x)),
    vp9_decoder(std::make_unique<Decoder::VP9>(host1x))
{
}

Codec::~Codec() = default;

void Codec::Initialize()
{
    initialized = decode_api.Initialize(current_codec);
}

void Codec::SetTargetCodec(Host1x::NvdecCommon::VideoCodec codec)
{
    if (current_codec != codec)
    {
        // A codec switch must not reuse the previous FFmpeg decoder or its frames.
        initialized = false;
        decode_api.Reset();
        // VP9 reconstruction caches reference/probability state across frames.
        // A new stream must not inherit it after H.264/VP8 -> VP9 transitions.
        h264_decoder = std::make_unique<Decoder::H264>(host1x);
        vp8_decoder = std::make_unique<Decoder::VP8>(host1x);
        vp9_decoder = std::make_unique<Decoder::VP9>(host1x);
        while (!frames.empty())
            frames.pop();
        current_codec = codec;
        LOG_INFO(Service_NVDRV, "NVDEC video codec initialized to {}", GetCurrentCodecName());
    }
}

void Codec::Decode()
{
    const bool is_first_frame = !initialized;
    if (is_first_frame)
    {
        Initialize();
    }

    if (!initialized)
    {
        return;
    }

    // Assemble bitstream.
    size_t configuration_size = 0;
    FFmpeg::FrameInfo frame_info{};
    const auto packet_data = [&]() {
        switch (current_codec) {
        case Tegra::Host1x::NvdecCommon::VideoCodec::H264:
        {
            const auto packet = h264_decoder->ComposeFrame(state, &configuration_size, is_first_frame);
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_MEDIACODEC)
            const auto [width, height] = h264_decoder->GetFrameDimensions();
            frame_info = {width, height};
#endif
            return packet;
        }
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP8:
            return vp8_decoder->ComposeFrame(state);
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP9:
            vp9_decoder->ComposeFrame(state);
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_MEDIACODEC)
            {
                const auto [width, height] = vp9_decoder->GetFrameDimensions();
                frame_info = {width, height, vp9_decoder->IsKeyFrame()};
            }
#endif
            return vp9_decoder->GetFrameBytes();
        default:
            ASSERT(false);
            return std::span<const u8>{};
        }
    }();

    if (!decode_api.SendPacket(packet_data, configuration_size, frame_info, frames))
        return;
    // A hidden VP9 input can unblock earlier visible output. FFmpeg suppresses
    // the hidden image itself, so always drain the available decoded frames.
    decode_api.ReceiveFrames(frames);
}

std::unique_ptr<FFmpeg::Frame> Codec::GetCurrentFrame() {
    // Sometimes VIC will request more frames than have been decoded.
    // in this case, return a blank frame and don't overwrite previous data.
    if (frames.empty()) {
        return {};
    }

    auto frame = std::move(frames.front());
    frames.pop();
    return frame;
}

Host1x::NvdecCommon::VideoCodec Codec::GetCurrentCodec() const
{
    return current_codec;
}

std::string_view Codec::GetCurrentCodecName() const
{
    switch (current_codec)
    {
    case Host1x::NvdecCommon::VideoCodec::None:
        return "None";
    case Host1x::NvdecCommon::VideoCodec::H264:
        return "H264";
    case Host1x::NvdecCommon::VideoCodec::VP8:
        return "VP8";
    case Host1x::NvdecCommon::VideoCodec::H265:
        return "H265";
    case Host1x::NvdecCommon::VideoCodec::VP9:
        return "VP9";
    default:
        return "Unknown";
    }
}
} // namespace Tegra
