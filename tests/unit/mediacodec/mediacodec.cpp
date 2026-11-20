// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <unordered_set>
#include "hooks.h"
#include "fixtures.h"
#include "video_settings.h"
#include "yuzu_video_core/host1x/codecs/codec.h"
#include "yuzu_video_core/host1x/codecs/state.h"
#undef avcodec_find_decoder_by_name
#undef avcodec_open2
#undef avcodec_send_packet
#undef avcodec_receive_frame
#undef av_opt_set_int
#undef av_hwframe_transfer_data

VideoSettings videoSettings{};
using Codec = Tegra::Host1x::NvdecCommon::VideoCodec;
using namespace MediaCodecTest;
namespace {
bool available = true, real_codec = false;
int open_error = 0, option_error = 0, opened = 0, received = 0;
int transfer_error = 0;
std::vector<std::string> names;
std::vector<int64_t> timestamps;
std::vector<std::vector<u8>> packets;
std::deque<int> send_results;
struct Output { int result; AVPixelFormat format = AV_PIX_FMT_NV12; int width=64,height=32; bool interlaced=false; bool hardware=false; };
std::deque<Output> outputs;
std::unordered_set<void*> ndk_options;
std::vector<std::vector<u8>> extra;
void Reset() {
    available=true; real_codec=false; open_error=option_error=opened=received=0;
    names.clear(); timestamps.clear(); packets.clear(); send_results.clear();
    outputs.clear(); ndk_options.clear(); extra.clear(); transfer_error=0;
    videoSettings.nvdec_emulation=NvdecEmulation::Gpu;
}
std::queue<std::unique_ptr<FFmpeg::Frame>> frames;
constexpr FFmpeg::FrameInfo Info{64,32,true};
bool Send(FFmpeg::DecodeApi& api) { return api.SendPacket(H264,H264ConfigurationSize,Info,frames); }
void Start(FFmpeg::DecodeApi& api) {
    assert(api.Initialize(Codec::H264));
    assert(opened==0); // MediaCodec opening must wait for dimensions/SPS/PPS.
    assert(Send(api));
    assert(opened==1);
}
}
extern "C" const AVCodec* NxemuMcFind(const char* name) {
    names.emplace_back(name);
    if (!available) return nullptr;
    return avcodec_find_decoder(!std::strcmp(name,"h264_mediacodec") ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP9);
}
extern "C" int NxemuMcOption(void* obj,const char* name,int64_t value,int flags) {
    if (!std::strcmp(name,"ndk_codec")) {
        assert(value==1);
        if (option_error == 0) ndk_options.insert(obj);
        return option_error;
    }
    return av_opt_set_int(obj,name,value,flags);
}
extern "C" int NxemuMcOpen(AVCodecContext* ctx,const AVCodec* codec,AVDictionary** dict) {
    ++opened;
    const bool hardware=ndk_options.erase(ctx->priv_data)!=0;
    if (hardware) {
        assert(ctx->width>0 && ctx->height>0 && ctx->coded_width==ctx->width);
        assert(ctx->pkt_timebase.num==1 && ctx->pkt_timebase.den==1'000'000);
    }
    std::vector<u8> config;
    if (ctx->extradata_size) {
        config.assign(ctx->extradata,ctx->extradata+ctx->extradata_size);
        for (int i=0;i<AV_INPUT_BUFFER_PADDING_SIZE;++i)
            assert(ctx->extradata[ctx->extradata_size+i]==0);
    }
    extra.push_back(std::move(config));
    if (hardware && open_error) return open_error;
    return real_codec ? avcodec_open2(ctx,codec,dict) : 0;
}
extern "C" int NxemuMcSend(AVCodecContext* ctx,const AVPacket* packet) {
    assert(packet && packet->buf && packet->size>0);
    for (int i=0;i<AV_INPUT_BUFFER_PADDING_SIZE;++i)
        assert(packet->data[packet->size+i]==0);
    timestamps.push_back(packet->pts);
    packets.emplace_back(packet->data,packet->data+packet->size);
    if (real_codec) return avcodec_send_packet(ctx,packet);
    if (send_results.empty()) return 0;
    const int result=send_results.front(); send_results.pop_front(); return result;
}
extern "C" int NxemuMcReceive(AVCodecContext* ctx,AVFrame* frame) {
    ++received;
    if (real_codec) return avcodec_receive_frame(ctx,frame);
    if (outputs.empty()) return AVERROR(EAGAIN);
    const auto output=outputs.front(); outputs.pop_front();
    if (output.result<0) return output.result;
    frame->format=output.format; frame->width=output.width; frame->height=output.height;
    frame->pts=received;
    if (output.interlaced) frame->flags|=AV_FRAME_FLAG_INTERLACED;
    assert(av_frame_get_buffer(frame,32)==0);
    for (const auto* buffer : frame->buf)
        if (buffer) std::memset(buffer->data,0,buffer->size);
    if (output.hardware)
        frame->hw_frames_ctx=av_buffer_alloc(64);
    return 0;
}
extern "C" int NxemuMcTransfer(AVFrame* dst,const AVFrame* src,int flags) {
    return transfer_error ? transfer_error : av_hwframe_transfer_data(dst,src,flags);
}

int main() {
    Reset();
    { // Only H.264/VP9 may select MediaCodec; CPU and VP8 remain standard FFmpeg.
        FFmpeg::Decoder h264(Codec::H264,true),vp9(Codec::VP9,true),vp8(Codec::VP8,true),cpu(Codec::H264);
        assert(h264.IsAndroidMediaCodec() && vp9.IsAndroidMediaCodec());
        assert(!vp8.IsAndroidMediaCodec() && !cpu.IsAndroidMediaCodec());
        assert((names==std::vector<std::string>{"h264_mediacodec","vp9_mediacodec"}));
        available=false;
        FFmpeg::Decoder missing(Codec::H264,true);
        assert(!missing.IsAndroidMediaCodec() && missing.GetCodec());
    }
    Reset();
    { // Own the caller's bytes and the padding FFmpeg requires.
        std::vector<u8> input{1,2,3};
        FFmpeg::Packet packet(input); input[0]=99;
        assert(packet.GetPacket()->data[0]==1 && packet.GetPacket()->buf);
        FFmpeg::Packet empty({}); assert(!empty.IsValid());
    }
    Reset();
    {
        FFmpeg::DecodeApi api; Start(api);
        assert(extra[0].size()==H264ConfigurationSize);
        assert(!api.SendPacket({},0,Info,frames));
        assert(!api.SendPacket(H264,H264.size()+1,Info,frames));
        send_results={AVERROR(EAGAIN),0};
        outputs={{0},{0},{AVERROR(EAGAIN)}};
        assert(Send(api));
        assert(frames.size()==2 && timestamps.size()==3);
        assert(timestamps[1]==1 && timestamps[2]==1 && packets[1]==packets[2]);
        outputs.assign(20,Output{0}); api.ReceiveFrames(frames);
        assert(frames.size()==10); // Drain is complete, queue remains bounded.
        assert(Send(api) && timestamps.back()==2);
        assert(api.SendPacket(H264,H264ConfigurationSize,{},frames));
        assert(opened==1); // Omitted later metadata reuses the known native size.
    }
    while(!frames.empty()) frames.pop();
    Reset();
    {
        FFmpeg::DecodeApi api; assert(api.Initialize(Codec::H264));
        assert(api.SendPacket(H264,H264ConfigurationSize,{0,32,true},frames));
        assert(opened==1); // Only software was opened; no invalid native dimensions.
        api.Reset(); assert(api.Initialize(Codec::H264));
        assert(api.SendPacket(H264,H264ConfigurationSize,{64,16385,true},frames));
        assert(opened==2);
    }
    Reset();
    { // Failed open keeps parameter sets and retries the same independent input in software.
        FFmpeg::DecodeApi api; assert(api.Initialize(Codec::H264));
        open_error=AVERROR_EXTERNAL;
        assert(Send(api) && opened==2 && extra[0]==extra[1]);
    }
    Reset();
    { // No NDK backend must not fall into an unregistered Java MediaCodec backend.
        FFmpeg::DecodeApi api; assert(api.Initialize(Codec::H264));
        option_error=AVERROR_OPTION_NOT_FOUND;
        assert(Send(api) && opened==1);
    }
    Reset();
    { // A second EAGAIN is not allowed to discard the independent input.
        FFmpeg::DecodeApi api; Start(api);
        send_results={AVERROR(EAGAIN),AVERROR(EAGAIN),0};
        assert(Send(api) && opened==2 && timestamps.back()==1);
        assert(packets[1]==packets[2] && packets[2]==packets[3]);
    }
    Reset();
    { // Retrieval/format failure clears old output, then waits for an independent frame.
        FFmpeg::DecodeApi api; Start(api);
        outputs={{0},{0,AV_PIX_FMT_RGBA}};
        api.ReceiveFrames(frames); assert(frames.empty() && opened==2);
        const std::array<u8,5> dependent{0,0,0,1,0x41};
        const auto previous=packets.size();
        assert(api.SendPacket(dependent,0,Info,frames) && packets.size()==previous);
        assert(Send(api) && packets.size()==previous+1);
        outputs={{0,AV_PIX_FMT_YUV420P10LE}};
        api.ReceiveFrames(frames);
        assert(!frames.empty() && frames.front()->GetPixelFormat()==AV_PIX_FMT_YUV420P);
    }
    while(!frames.empty()) frames.pop();
    Reset();
    {
        FFmpeg::DecodeApi api; Start(api);
        outputs={{AVERROR_EXTERNAL}}; api.ReceiveFrames(frames);
        assert(opened==2 && frames.empty());
        assert(Send(api));
    }
    Reset();
    { // Hardware-frame transfer failure is treated as a decoder failure.
        FFmpeg::DecodeApi api; Start(api);
        transfer_error=AVERROR_EXTERNAL;
        outputs={{0,AV_PIX_FMT_NV12,64,32,false,true}};
        api.ReceiveFrames(frames);
        assert(frames.empty() && opened==2);
    }
    Reset();
    { // A new resolution recreates MediaCodec, and Reset removes all stream metadata.
        FFmpeg::DecodeApi api; Start(api);
        assert(api.SendPacket(H264,H264ConfigurationSize,{128,64,true},frames));
        assert(opened==2);
        api.Reset(); assert(api.Initialize(Codec::VP9));
        assert(api.SendPacket(VP9,0,Info,frames) && extra.back().empty());
        assert(timestamps.back()==0);
    }
    Reset();
    { // Exercise real FFmpeg software decoding, not only the API doubles.
        real_codec=true; videoSettings.nvdec_emulation=NvdecEmulation::Cpu;
        FFmpeg::DecodeApi api;
        assert(api.Initialize(Codec::H264) && Send(api));
        api.ReceiveFrames(frames); assert(!frames.empty());
        assert(frames.front()->GetWidth()==64 && frames.front()->GetHeight()==32);
        while(!frames.empty()) frames.pop();
        assert(api.Initialize(Codec::VP9) && api.SendPacket(VP9,0,Info,frames));
        api.ReceiveFrames(frames); assert(!frames.empty());
    }
    while(!frames.empty()) frames.pop();
    Reset();
    { // Failed hardware startup must also work with the real software decoder.
        FFmpeg::DecodeApi api; real_codec=true; open_error=AVERROR_EXTERNAL;
        assert(api.Initialize(Codec::H264) && Send(api));
        api.ReceiveFrames(frames); assert(!frames.empty() && opened==2);
    }
    while(!frames.empty()) frames.pop();
    Reset();
    { // Recreate the real deinterlacer when the decoded size changes.
        FFmpeg::DecodeApi api; Start(api);
        outputs.assign(3,Output{0,AV_PIX_FMT_YUV420P,64,32,true});
        api.ReceiveFrames(frames); assert(!frames.empty());
        while(!frames.empty()) frames.pop();
        outputs.assign(3,Output{0,AV_PIX_FMT_YUV420P,128,64,true});
        api.ReceiveFrames(frames); assert(!frames.empty());
        assert(frames.front()->GetWidth()==128 && frames.front()->GetHeight()==64);
    }
    while(!frames.empty()) frames.pop();
    Reset();
    { // Real Codec switching must reset VP9 reconstruction and stale decoded images.
        Tegra::Host1x::Host1x host;
        Tegra::Host1x::NvdecCommon::NvdecRegisters regs{};
        Tegra::Codec codec(host,regs);
        codec.SetTargetCodec(Codec::H264); outputs={{0}}; codec.Decode();
        codec.SetTargetCodec(Codec::VP9); assert(!codec.GetCurrentFrame());
        const int generation=vp9_instances;
        outputs={{0}}; codec.Decode(); assert(codec.GetCurrentFrame());
        assert(vp9_compositions==1);
        codec.SetTargetCodec(Codec::H264); codec.SetTargetCodec(Codec::VP9);
        assert(vp9_instances>generation && vp9_compositions==0);
    }
}
