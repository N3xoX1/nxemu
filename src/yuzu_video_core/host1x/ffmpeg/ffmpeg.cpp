// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// MediaCodec integration adapted for NXEmu on 2026-10-30.

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <mutex>
#include <cerrno>
#include <cstring>
#include <limits>

#include "yuzu_video_core/host1x/ffmpeg/ffmpeg.h"
#include "yuzu_video_core/host1x/ffmpeg/device_cache.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_common/yuzu_assert.h"

extern "C"
{
#include <libswscale/swscale.h>
#ifdef LIBVA_FOUND
// for querying VAAPI driver information
#include <libavutil/hwcontext_vaapi.h>
#endif
}

namespace FFmpeg
{

namespace
{

std::mutex device_cache_mutex;
std::map<AVHWDeviceType, AVBufferRef *> device_cache;
int GetCachedDevice(AVBufferRef ** output, AVHWDeviceType type)
{
    std::scoped_lock lock{device_cache_mutex};
    auto & device = device_cache[type];
    if (!device)
    {
        const auto ret = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
        if (ret < 0)
        {
            device_cache.erase(type);
            return ret;
        }
    }
    *output = av_buffer_ref(device);
    return *output ? 0 : AVERROR(ENOMEM);
}
constexpr AVPixelFormat PreferredGpuFormat = AV_PIX_FMT_NV12;
constexpr AVPixelFormat PreferredCpuFormat = AV_PIX_FMT_YUV420P;
constexpr std::array PreferredGpuDecoders = {
#ifdef __APPLE__
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
    AV_HWDEVICE_TYPE_CUDA,
#ifdef _WIN32
    AV_HWDEVICE_TYPE_D3D11VA,
    AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__unix__) && !defined(__APPLE__)
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
#endif
    // last resort for Linux Flatpak (w/ NVIDIA)
    AV_HWDEVICE_TYPE_VULKAN,
};

AVPixelFormat GetGpuFormat(AVCodecContext * codec_context, const AVPixelFormat * pix_fmts)
{
    // FFmpeg can change pix_fmt before invoking get_format; retain our selection separately.
    auto & hw_pix_fmt = *static_cast<AVPixelFormat *>(codec_context->opaque);
    for (const AVPixelFormat * p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == hw_pix_fmt)
        {
            return hw_pix_fmt;
        }
    }

    LOG_INFO(HW_GPU, "Could not find compatible GPU AV format, falling back to CPU");
    av_buffer_unref(&codec_context->hw_device_ctx);
    hw_pix_fmt = AV_PIX_FMT_NONE;

    for (const AVPixelFormat * p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == PreferredCpuFormat)
        {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

std::string AVError(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(errbuf, sizeof(errbuf) - 1, errnum);
    return errbuf;
}

} // namespace

void WarmHardwareDecoderDevices()
{
    if (videoSettings.nvdec_emulation != NvdecEmulation::Gpu) return;
    Decoder decoder{Tegra::Host1x::NvdecCommon::VideoCodec::H264, true};
    // MediaCodec initializes its decoder from the first frame's parameters.
    if (decoder.IsAndroidMediaCodec()) return;
    if (decoder.GetCodec())
    {
        DecoderContext context{decoder};
        HardwareContext hardware;
        hardware.InitializeForDecoder(context, decoder);
    }
}
void ReleaseHardwareDecoderDevices()
{
    std::scoped_lock lock{device_cache_mutex};
    for (auto & [type, device] : device_cache)
        av_buffer_unref(&device);
    device_cache.clear();
}
Packet::Packet(std::span<const u8> data)
{
    if (data.empty() || data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return;
    m_packet = av_packet_alloc();
    if (!m_packet)
        return;
    // FFmpeg may read AV_INPUT_BUFFER_PADDING_SIZE bytes beyond the bitstream.
    // Own a padded copy: MediaCodec/FFmpeg may also retain it after SendPacket.
    if (av_new_packet(m_packet, static_cast<int>(data.size())) < 0)
    {
        av_packet_free(&m_packet);
        return;
    }
    std::memcpy(m_packet->data, data.data(), data.size());
}

Packet::~Packet()
{
    av_packet_free(&m_packet);
}

Frame::Frame()
{
    m_frame = av_frame_alloc();
}

Frame::~Frame()
{
    av_frame_free(&m_frame);
}

Decoder::Decoder(Tegra::Host1x::NvdecCommon::VideoCodec codec, bool prefer_android_mediacodec)
{
    const AVCodecID av_codec = [&] {
        switch (codec)
        {
        case Tegra::Host1x::NvdecCommon::VideoCodec::H264:
            return AV_CODEC_ID_H264;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP8:
            return AV_CODEC_ID_VP8;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP9:
            return AV_CODEC_ID_VP9;
        default:
            UNIMPLEMENTED_MSG("Unknown codec {}", codec);
            return AV_CODEC_ID_NONE;
        }
    }();

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_MEDIACODEC)
    if (prefer_android_mediacodec)
    {
        // VP8 MediaCodec has known game-specific crashes. Keep it on FFmpeg software.
        const char *mediacodec_name = nullptr;
        if (av_codec == AV_CODEC_ID_H264)
            mediacodec_name = "h264_mediacodec";
        else if (av_codec == AV_CODEC_ID_VP9)
            mediacodec_name = "vp9_mediacodec";

        if (mediacodec_name)
        {
            m_codec = avcodec_find_decoder_by_name(mediacodec_name);
            m_android_mediacodec = m_codec != nullptr;
        }
    }
#else
    (void)prefer_android_mediacodec;
#endif
    if (!m_codec)
        m_codec = avcodec_find_decoder(av_codec);
}

bool Decoder::SupportsDecodingOnDevice(AVPixelFormat * out_pix_fmt, AVHWDeviceType type) const
{
    for (int i = 0;; i++)
    {
        const AVCodecHWConfig * config = avcodec_get_hw_config(m_codec, i);
        if (!config)
        {
            LOG_DEBUG(HW_GPU, "{} decoder does not support device type {}", m_codec->name, av_hwdevice_get_type_name(type));
            break;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && config->device_type == type)
        {
            LOG_INFO(HW_GPU, "Using {} GPU decoder", av_hwdevice_get_type_name(type));
            *out_pix_fmt = config->pix_fmt;
            return true;
        }
    }

    return false;
}

std::vector<AVHWDeviceType> HardwareContext::GetSupportedDeviceTypes()
{
    std::vector<AVHWDeviceType> types;
    AVHWDeviceType current_device_type = AV_HWDEVICE_TYPE_NONE;

    while (true)
    {
        current_device_type = av_hwdevice_iterate_types(current_device_type);
        if (current_device_type == AV_HWDEVICE_TYPE_NONE)
        {
            return types;
        }

        types.push_back(current_device_type);
    }
}

HardwareContext::~HardwareContext()
{
    av_buffer_unref(&m_gpu_decoder);
}

bool HardwareContext::InitializeForDecoder(DecoderContext & decoder_context, const Decoder & decoder)
{
    if (!decoder_context.GetCodecContext())
        return false;
    const auto supported_types = GetSupportedDeviceTypes();
    for (const auto type : PreferredGpuDecoders)
    {
        AVPixelFormat hw_pix_fmt;

        if (std::ranges::find(supported_types, type) == supported_types.end())
        {
            LOG_DEBUG(HW_GPU, "{} explicitly unsupported", av_hwdevice_get_type_name(type));
            continue;
        }

        if (!this->InitializeWithType(type))
        {
            continue;
        }

        if (decoder.SupportsDecodingOnDevice(&hw_pix_fmt, type))
        {
            decoder_context.InitializeHardwareDecoder(*this, hw_pix_fmt);
            return true;
        }
    }

    return false;
}

bool HardwareContext::InitializeWithType(AVHWDeviceType type)
{
    av_buffer_unref(&m_gpu_decoder);
    const int ret = GetCachedDevice(&m_gpu_decoder, type);
    if (ret < 0)
    {
        LOG_DEBUG(HW_GPU, "av_hwdevice_ctx_create({}) failed: {}", av_hwdevice_get_type_name(type), AVError(ret));
        return false;
    }

#ifdef LIBVA_FOUND
    if (type == AV_HWDEVICE_TYPE_VAAPI)
    {
        // We need to determine if this is an impersonated VAAPI driver.
        auto * hwctx = reinterpret_cast<AVHWDeviceContext *>(m_gpu_decoder->data);
        auto * vactx = static_cast<AVVAAPIDeviceContext *>(hwctx->hwctx);
        const char * vendor_name = vaQueryVendorString(vactx->display);
        if (strstr(vendor_name, "VDPAU backend"))
        {
            // VDPAU impersonated VAAPI impls are super buggy, we need to skip them.
            LOG_DEBUG(HW_GPU, "Skipping VDPAU impersonated VAAPI driver");
            return false;
        }
        else
        {
            // According to some user testing, certain VAAPI drivers (Intel?) could be buggy.
            // Log the driver name just in case.
            LOG_DEBUG(HW_GPU, "Using VAAPI driver: {}", vendor_name);
        }
    }
#endif
    return true;
}

DecoderContext::DecoderContext(const Decoder & decoder)
{
    m_codec_context = avcodec_alloc_context3(decoder.GetCodec());
    if (!m_codec_context)
        return;
    av_opt_set(m_codec_context->priv_data, "tune", "zerolatency", 0);
    m_codec_context->thread_count = 0;
    m_codec_context->thread_type &= ~FF_THREAD_FRAME;
}

DecoderContext::~DecoderContext()
{
    if (m_codec_context)
        av_buffer_unref(&m_codec_context->hw_device_ctx);
    avcodec_free_context(&m_codec_context);
}

void DecoderContext::InitializeHardwareDecoder(const HardwareContext & context, AVPixelFormat hw_pix_fmt)
{
    m_codec_context->hw_device_ctx = av_buffer_ref(context.GetBufferRef());
    m_hw_pix_fmt = hw_pix_fmt;
    m_codec_context->opaque = &m_hw_pix_fmt;
    m_codec_context->get_format = GetGpuFormat;
    m_codec_context->pix_fmt = hw_pix_fmt;
}

bool DecoderContext::OpenContext(const Decoder & decoder, std::span<const u8> extradata)
{
    if (!m_codec_context)
        return false;
    if (decoder.IsAndroidMediaCodec())
    {
        // Use the NDK backend explicitly. FFmpeg lives in the dynamically loaded
        // video module, independently of the frontend's JavaVM registration.
        if (av_opt_set_int(m_codec_context->priv_data, "ndk_codec", 1, 0) < 0)
            return false;
        m_codec_context->pkt_timebase = AVRational{1, 1'000'000};
    }

    if (!extradata.empty())
    {
        if (extradata.size() > static_cast<size_t>(std::numeric_limits<int>::max()) -
                                   AV_INPUT_BUFFER_PADDING_SIZE)
        {
            LOG_ERROR(HW_GPU, "FFmpeg extradata is too large");
            return false;
        }
        auto *buffer = static_cast<u8 *>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!buffer)
        {
            LOG_ERROR(HW_GPU, "FFmpeg extradata allocation failed");
            return false;
        }
        av_freep(&m_codec_context->extradata);
        std::memcpy(buffer, extradata.data(), extradata.size());
        m_codec_context->extradata = buffer;
        m_codec_context->extradata_size = static_cast<int>(extradata.size());
    }
    if (const int ret = avcodec_open2(m_codec_context, decoder.GetCodec(), nullptr); ret < 0)
    {
        LOG_ERROR(HW_GPU, "avcodec_open2 error: {}", AVError(ret));
        return false;
    }

    if (!decoder.IsAndroidMediaCodec() && !m_codec_context->hw_device_ctx)
    {
        LOG_INFO(HW_GPU, "Using FFmpeg software decoding");
    }

    return true;
}

int DecoderContext::SendPacket(const Packet & packet)
{
    if (!m_codec_context || !packet.IsValid())
        return AVERROR(EINVAL);
    const int ret = avcodec_send_packet(m_codec_context, packet.GetPacket());
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        LOG_ERROR(HW_GPU, "avcodec_send_packet error: {}", AVError(ret));
    return ret;
}

std::unique_ptr<Frame> DecoderContext::ReceiveFrame(bool * out_is_interlaced, bool * out_decode_error)
{
    if (out_decode_error)
        *out_decode_error = false;
    auto dst_frame = std::make_unique<Frame>();

    const auto ReceiveImpl = [&](AVFrame * frame) {
        if (const int ret = avcodec_receive_frame(m_codec_context, frame); ret < 0)
        {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
            {
                LOG_ERROR(HW_GPU, "avcodec_receive_frame error: {}", AVError(ret));
                if (out_decode_error)
                    *out_decode_error = true;
            }
            return false;
        }

        *out_is_interlaced =
#if defined(FF_API_INTERLACED_FRAME) || LIBAVUTIL_VERSION_MAJOR >= 59
            (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
            frame->interlaced_frame != 0;
#endif
        return true;
    };

    if (!ReceiveImpl(dst_frame->GetFrame()))
    {
        return {};
    }

    // get_format may have fallen back to software while receiving this frame.
    // Transfer only frames that actually own a hardware surface.
    if (dst_frame->GetFrame()->hw_frames_ctx)
    {
        auto hardware_frame = std::move(dst_frame);
        dst_frame = std::make_unique<Frame>();
        dst_frame->SetFormat(PreferredGpuFormat);
        const int ret = av_hwframe_transfer_data(dst_frame->GetFrame(), hardware_frame->GetFrame(), 0);
        if (ret < 0)
        {
            LOG_ERROR(HW_GPU, "av_hwframe_transfer_data error: {}", AVError(ret));
            if (out_decode_error)
                *out_decode_error = true;
            return {};
        }
        if (av_frame_copy_props(dst_frame->GetFrame(), hardware_frame->GetFrame()) < 0)
        {
            if (out_decode_error)
                *out_decode_error = true;
            return {};
        }
    }

    return dst_frame;
}

DeinterlaceFilter::DeinterlaceFilter(const Frame & frame)
{
    const AVFilter * buffer_src = avfilter_get_by_name("buffer");
    const AVFilter * buffer_sink = avfilter_get_by_name("buffersink");
    AVFilterInOut * inputs = avfilter_inout_alloc();
    AVFilterInOut * outputs = avfilter_inout_alloc();
    SCOPE_EXIT
    {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    };

    // Don't know how to get the accurate time_base but it doesn't matter for yadif filter
    // so just use 1/1 to make buffer filter happy
    std::string args = fmt::format("video_size={}x{}:pix_fmt={}:time_base=1/1", frame.GetWidth(), frame.GetHeight(), static_cast<int>(frame.GetPixelFormat()));

    m_filter_graph = avfilter_graph_alloc();
    int ret = avfilter_graph_create_filter(&m_source_context, buffer_src, "in", args.c_str(), nullptr, m_filter_graph);
    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "avfilter_graph_create_filter source error: {}", AVError(ret));
        return;
    }
    ret = avfilter_graph_create_filter(&m_sink_context, buffer_sink, "out", nullptr, nullptr, m_filter_graph);
    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "avfilter_graph_create_filter sink error: {}", AVError(ret));
        return;
    }

    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_sink_context;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_source_context;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    const char * description = "yadif=1:-1:0";
    ret = avfilter_graph_parse_ptr(m_filter_graph, description, &inputs, &outputs, nullptr);
    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "avfilter_graph_parse_ptr error: {}", AVError(ret));
        return;
    }

    ret = avfilter_graph_config(m_filter_graph, nullptr);
    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "avfilter_graph_config error: {}", AVError(ret));
        return;
    }

    m_initialized = true;
}

bool DeinterlaceFilter::AddSourceFrame(const Frame & frame)
{
    if (!m_initialized)
        return false;
    const int ret = av_buffersrc_add_frame_flags(m_source_context, frame.GetFrame(), AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "av_buffersrc_add_frame_flags error: {}", AVError(ret));
        return false;
    }

    return true;
}

std::unique_ptr<Frame> DeinterlaceFilter::DrainSinkFrame()
{
    if (!m_initialized)
        return {};
    auto dst_frame = std::make_unique<Frame>();
    const int ret = av_buffersink_get_frame(m_sink_context, dst_frame->GetFrame());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        return {};
    }

    if (ret < 0)
    {
        LOG_ERROR(HW_GPU, "av_buffersink_get_frame error: {}", AVError(ret));
        return {};
    }

    return dst_frame;
}

DeinterlaceFilter::~DeinterlaceFilter()
{
    avfilter_graph_free(&m_filter_graph);
}

void DecodeApi::Reset()
{
    m_deinterlace_filter.reset();
    m_hardware_context.reset();
    m_decoder_context.reset();
    m_decoder.reset();
    m_opened = false;
    m_android_mediacodec = false;
    m_waiting_for_keyframe = false;
    m_software_fallback = false;
    m_frame_info = {};
    m_filter_info = {};
    m_filter_format = AV_PIX_FMT_NONE;
    m_next_packet_pts = 0;
    m_h264_parameter_sets.clear();
}

bool DecodeApi::SwitchToSoftwareDecoder(std::queue<std::unique_ptr<Frame>>& frame_queue)
{
    m_deinterlace_filter.reset();
    m_decoder_context.reset();
    m_hardware_context.reset();
    m_decoder.reset();
    m_android_mediacodec = false;
    m_opened = false;
    m_waiting_for_keyframe = true;
    m_software_fallback = true;
    // Hardware output and references belong to the old decoding epoch.
    while (!frame_queue.empty())
        frame_queue.pop();
    m_decoder.emplace(m_codec);
    if (!m_decoder->GetCodec())
        return false;
    m_decoder_context.emplace(*m_decoder);
    auto* context = m_decoder_context->GetCodecContext();
    if (!context)
        return false;
    context->width = static_cast<int>(m_frame_info.width);
    context->height = static_cast<int>(m_frame_info.height);
    m_opened = m_decoder_context->OpenContext(*m_decoder, m_h264_parameter_sets);
    if (m_opened)
    {
        LOG_INFO(HW_GPU, "Android MediaCodec unavailable, using FFmpeg software decoder");
    }
    return m_opened;
}

bool DecodeApi::Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec)
{
    this->Reset();
    m_codec = codec;
    m_decoder.emplace(codec, videoSettings.nvdec_emulation == NvdecEmulation::Gpu);
    if (!m_decoder->GetCodec())
    {
        LOG_ERROR(HW_GPU, "FFmpeg build has no decoder for codec {}", codec);
        this->Reset();
        return false;
    }
    m_decoder_context.emplace(*m_decoder);
    if (!m_decoder_context->GetCodecContext())
    {
        this->Reset();
        return false;
    }
    m_android_mediacodec = m_decoder->IsAndroidMediaCodec();

    // MediaCodec requires the dimensions and H.264 SPS/PPS from the first packet.
    if (m_android_mediacodec)
        return true;

    // Enable GPU decoding if requested.
    if (videoSettings.nvdec_emulation == NvdecEmulation::Gpu)
    {
        m_hardware_context.emplace();
        m_hardware_context->InitializeForDecoder(*m_decoder_context, *m_decoder);
    }

    // Open the decoder context.
    if (!m_decoder_context->OpenContext(*m_decoder))
    {
        this->Reset();
        return false;
    }

    m_opened = true;

    return true;
}

namespace {

bool HasH264IDR(std::span<const u8> packet)
{
    // Both three- and four-byte Annex B prefixes are recognized by their tail.
    for (size_t i = 2; i + 1 < packet.size(); ++i)
        if (packet[i - 2] == 0 && packet[i - 1] == 0 && packet[i] == 1 &&
            (packet[i + 1] & 0x1f) == 5)
            return true;
    return false;
}

} // namespace

bool DecodeApi::SendPacket(std::span<const u8> packet_data, size_t configuration_size,
                           FrameInfo info, std::queue<std::unique_ptr<Frame>>& frame_queue)
{
    if (!m_decoder_context || packet_data.empty() || configuration_size > packet_data.size())
        return false;
    const bool was_mediacodec = m_android_mediacodec;
    const auto is_keyframe = [&] {
        return m_codec == Tegra::Host1x::NvdecCommon::VideoCodec::H264
                   ? HasH264IDR(packet_data) : info.key_frame;
    };
    if (m_android_mediacodec)
    {
        constexpr u32 MaxVideoDimension = 16384;
        if (m_codec == Tegra::Host1x::NvdecCommon::VideoCodec::H264 && configuration_size > 0)
            m_h264_parameter_sets.assign(packet_data.begin(),
                                         packet_data.begin() + configuration_size);
        // Once opened, an omitted size does not invalidate the decoder's known size.
        if (m_opened)
        {
            if (info.width == 0) info.width = m_frame_info.width;
            if (info.height == 0) info.height = m_frame_info.height;
        }
        if (info.width == 0 || info.height == 0 ||
            info.width > MaxVideoDimension || info.height > MaxVideoDimension)
        {
            // The software decoder can derive dimensions from the bitstream;
            // never pass invalid guest dimensions to the platform decoder.
            if (!SwitchToSoftwareDecoder(frame_queue))
                return false;
        }
        if (m_android_mediacodec && m_opened &&
            (info.width != m_frame_info.width || info.height != m_frame_info.height))
        {
            // Reconfigure only on an independent frame; old references cannot
            // describe a new-sized stream. Keep the previous displayed image until then.
            if (!is_keyframe())
                return true;
            m_deinterlace_filter.reset();
            m_decoder_context.reset();
            m_decoder_context.emplace(*m_decoder);
            m_opened = false;
            while (!frame_queue.empty())
                frame_queue.pop();
        }
        if (m_android_mediacodec)
            m_frame_info = info;
        if (m_android_mediacodec && !m_opened)
        {
            auto* context = m_decoder_context->GetCodecContext();
            const bool has_configuration =
                m_codec != Tegra::Host1x::NvdecCommon::VideoCodec::H264 ||
                !m_h264_parameter_sets.empty();
            if (context && has_configuration)
            {
                context->width = static_cast<int>(info.width);
                context->height = static_cast<int>(info.height);
                context->coded_width = context->width;
                context->coded_height = context->height;
                m_opened = m_decoder_context->OpenContext(*m_decoder, m_h264_parameter_sets);
            }
            if (m_opened)
            {
                LOG_INFO(HW_GPU, "Using Android MediaCodec decoder {}", m_decoder->GetCodec()->name);
            }
            else if (!SwitchToSoftwareDecoder(frame_queue))
                return false;
        }
    }
    if (!m_opened)
        return false;
    if (m_waiting_for_keyframe)
    {
        // Mid-stream replacement loses codec reference pictures. Sending dependent
        // frames to a fresh software decoder would produce corrupt or blank output.
        if (!is_keyframe())
            return true;
        m_waiting_for_keyframe = false;
    }
    FFmpeg::Packet packet(packet_data);
    if (!packet.IsValid())
        return false;
    if (was_mediacodec || m_android_mediacodec)
    {
        packet.GetPacket()->pts = m_next_packet_pts;
        packet.GetPacket()->dts = m_next_packet_pts;
    }
    int result = m_decoder_context->SendPacket(packet);
    if (result == AVERROR(EAGAIN))
    {
        // Drain ALL pending output, then resend the same owned packet and PTS.
        // FFmpeg forbids send and receive from both remaining in EAGAIN.
        ReceiveFrames(frame_queue);
        if (m_waiting_for_keyframe)
        {
            if (!is_keyframe())
                return true;
            m_waiting_for_keyframe = false;
        }
        result = m_decoder_context->SendPacket(packet);
    }
    if (result < 0 && m_android_mediacodec)
    {
        // This also handles a MediaCodec implementation violating the EAGAIN
        // contract, without dropping an independent input or looping indefinitely.
        LOG_WARNING(HW_GPU, "Android MediaCodec submission failed, trying software decoding");
        if (!SwitchToSoftwareDecoder(frame_queue))
            return false;
        if (!is_keyframe())
            return true;
        m_waiting_for_keyframe = false;
        result = m_decoder_context->SendPacket(packet);
    }
    if (result >= 0 && (was_mediacodec || m_android_mediacodec))
        ++m_next_packet_pts;
    return result >= 0;
}

void DecodeApi::QueueFrame(std::unique_ptr<Frame> frame,
                          std::queue<std::unique_ptr<Frame>>& frame_queue)
{
    if (m_software_fallback && frame->GetPixelFormat() != AV_PIX_FMT_NV12 &&
        frame->GetPixelFormat() != AV_PIX_FMT_YUV420P)
    {
        // A software replacement can return e.g. 10-bit planar output too.
        // VIC's YUV path only accepts NV12/YUV420P; never forward an incompatible
        // replacement frame to its plane-copy code.
        SwsContext* conversion = sws_getContext(frame->GetWidth(), frame->GetHeight(),
            frame->GetPixelFormat(), frame->GetWidth(), frame->GetHeight(),
            AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!conversion)
            return;
        SCOPE_EXIT { sws_freeContext(conversion); };
        auto converted = std::make_unique<Frame>();
        AVFrame* output = converted->GetFrame();
        output->width = frame->GetWidth();
        output->height = frame->GetHeight();
        output->format = AV_PIX_FMT_YUV420P;
        if (av_frame_get_buffer(output, 32) < 0 ||
            sws_scale(conversion, frame->GetPlanes(), frame->GetStrides(), 0,
                      frame->GetHeight(), converted->GetPlanes(), converted->GetStrides())
                != frame->GetHeight() ||
            av_frame_copy_props(output, frame->GetFrame()) < 0)
            return;
        frame = std::move(converted);
    }
    frame_queue.push(std::move(frame));
    // Keep the existing VIC queue bound even when draining multiple output frames.
    if (frame_queue.size() > 10)
        frame_queue.pop();
}

bool DecodeApi::ReceiveFrames(std::queue<std::unique_ptr<Frame>>& frame_queue)
{
    if (!m_decoder_context || !m_opened || m_waiting_for_keyframe)
        return false;
    bool received = false;
    for (;;)
    {
        bool is_interlaced = false;
        bool decode_error = false;
        auto frame = m_decoder_context->ReceiveFrame(&is_interlaced, &decode_error);
        if (!frame)
        {
            if (m_android_mediacodec && decode_error)
            {
                LOG_WARNING(HW_GPU, "Android MediaCodec retrieval failed, trying software decoding");
                SwitchToSoftwareDecoder(frame_queue);
            }
            return received;
        }
        received = true;
        if (m_android_mediacodec && frame->GetPixelFormat() != AV_PIX_FMT_NV12 &&
            frame->GetPixelFormat() != AV_PIX_FMT_YUV420P)
        {
            LOG_WARNING(HW_GPU, "Unsupported MediaCodec output format {}, trying software decoding",
                        static_cast<int>(frame->GetPixelFormat()));
            SwitchToSoftwareDecoder(frame_queue);
            return received;
        }
        if (!is_interlaced)
        {
            QueueFrame(std::move(frame), frame_queue);
            continue;
        }
        if (!m_deinterlace_filter || m_filter_info.width != static_cast<u32>(frame->GetWidth()) ||
            m_filter_info.height != static_cast<u32>(frame->GetHeight()) ||
            m_filter_format != frame->GetPixelFormat())
        {
            m_deinterlace_filter.reset();
            m_filter_info = {static_cast<u32>(frame->GetWidth()), static_cast<u32>(frame->GetHeight())};
            m_filter_format = frame->GetPixelFormat();
            m_deinterlace_filter.emplace(*frame);
        }
        if (!m_deinterlace_filter->AddSourceFrame(*frame))
            return received;
        while (auto output = m_deinterlace_filter->DrainSinkFrame())
            QueueFrame(std::move(output), frame_queue);
    }
}

} // namespace FFmpeg
