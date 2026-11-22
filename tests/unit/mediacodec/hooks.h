#pragma once
#include "yuzu_video_core/host1x/ffmpeg/ffmpeg.h"
extern "C" {
const AVCodec* NxemuMcFind(const char*);
int NxemuMcOpen(AVCodecContext*, const AVCodec*, AVDictionary**);
int NxemuMcSend(AVCodecContext*, const AVPacket*);
int NxemuMcReceive(AVCodecContext*, AVFrame*);
int NxemuMcOption(void*, const char*, int64_t, int);
int NxemuMcTransfer(AVFrame*, const AVFrame*, int);
}
// Only test objects use these substitutes; the real FFmpeg packet, context,
// frame and filter implementations remain linked for ownership and decode tests.
#define avcodec_find_decoder_by_name NxemuMcFind
#define avcodec_open2 NxemuMcOpen
#define avcodec_send_packet NxemuMcSend
#define avcodec_receive_frame NxemuMcReceive
#define av_opt_set_int NxemuMcOption
#define av_hwframe_transfer_data NxemuMcTransfer
