// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "core/core.h"
#include "core/core_timing.h"
#include "nxemu-os/os_settings.h"
#include "yuzu_audio_core/audio_core.h"
#include "yuzu_audio_core/common/common.h"
#include "yuzu_audio_core/sink/sink_stream.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/fixed_point.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"

namespace AudioCore::Sink
{

namespace
{
// Rounds to the nearest value: truncating would bias every sample towards zero.
s16 Clamp(f32 sample)
{
    constexpr f32 min = std::numeric_limits<s16>::min(), max = std::numeric_limits<s16>::max();
    return static_cast<s16>(std::lrint(std::clamp(sample, min, max)));
}

// Writes frames of `in_channels` samples as frames of `out_channels` samples (2 or 6).
// No upmix: a stereo source plays on the front speakers of a 5.1 output.
void ConvertFrames(const s16 * in, u32 in_channels, s16 * out, u32 out_channels, std::size_t frames)
{
    // Default 5.1 to stereo weights of the console: center and back -3 dB, LFE -12 dB.
    constexpr f32 front = 1.0f, center = 0.707f, lfe = 0.251f, back = 0.707f;
    for (std::size_t i = 0; i < frames; i++, in += in_channels, out += out_channels)
    {
        if (out_channels == 2 && in_channels == 6)
        {
            const f32 common = in[2] * center + in[3] * lfe;
            out[0] = Clamp(in[0] * front + common + in[4] * back);
            out[1] = Clamp(in[1] * front + common + in[5] * back);
            continue;
        }
        if (out_channels == 2 && in_channels == 4)
        {
            out[0] = Clamp(in[0] * front + in[2] * back);
            out[1] = Clamp(in[1] * front + in[3] * back);
            continue;
        }
        std::fill_n(out, out_channels, s16{0});
        switch (in_channels)
        {
        case 1:
            out[0] = out[1] = in[0];
            break;
        case 4: // Front and back pairs, into a 5.1 frame.
            out[0] = in[0];
            out[1] = in[1];
            out[4] = in[2];
            out[5] = in[3];
            break;
        default:
            std::copy_n(in, std::min(in_channels, out_channels), out);
            break;
        }
    }
}
} // namespace

void SinkStream::AppendBuffer(SinkBuffer & buffer, std::span<s16> samples)
{
    SCOPE_EXIT
    {
        queue.enqueue(buffer);
        ++queued_buffers;
    };

    if (type == StreamType::In)
    {
        return;
    }

    // Samples stay in the layout of the game; the callback converts them to the
    // device's, which can change while they wait.
    const u32 channels = GetSystemChannels();
    buffer.channels = channels;
    buffer.stored_frames = 0;
    if (channels == 0 || channels > MaxChannels)
    {
        return;
    }
    // The buffer keeps the length the game submitted: the played count must reach its end for
    // the game to get it back. Frames past the free room in the ring play as silence.
    const std::size_t free_frames = (samples_buffer.Capacity() - samples_buffer.Size()) / channels;
    buffer.stored_frames = std::min<u64>({buffer.frames, samples.size() / channels, free_frames});
    samples = samples.first(buffer.stored_frames * channels);

    f32 yuzu_volume = osSettings.audio_muted ? 0.0f : osSettings.volume / 100.0f;
    if (yuzu_volume > 1.0f)
    {
        yuzu_volume = 0.6f + 20 * std::log10(yuzu_volume);
    }
    const f32 volume = system_volume * device_volume * yuzu_volume;
    if (volume != 1.0f)
    {
        for (s16 & sample : samples)
        {
            sample = Clamp(static_cast<f32>(sample) * volume);
        }
    }

    samples_buffer.Push(samples);
}

std::vector<s16> SinkStream::ReleaseBuffer(u64 num_samples)
{
    constexpr s32 min = std::numeric_limits<s16>::min();
    constexpr s32 max = std::numeric_limits<s16>::max();

    auto samples{samples_buffer.Pop(num_samples)};

    // TODO: Up-mix to 6 channels if the game expects it.
    // For audio input this is unlikely to ever be the case though.

    // Incoming mic volume seems to always be very quiet, so multiply by an additional 8 here.
    // TODO: Play with this and find something that works better.
    auto volume{system_volume * device_volume * 8};
    for (u32 i = 0; i < samples.size(); i++)
    {
        samples[i] = static_cast<s16>(
            std::clamp(static_cast<s32>(static_cast<f32>(samples[i]) * volume), min, max));
    }

    if (samples.size() < num_samples)
    {
        samples.resize(num_samples, 0);
    }
    return samples;
}

void SinkStream::ClearQueue()
{
    samples_buffer.Pop();
    while (queue.pop())
    {
    }
    queued_buffers = 0;
    playing_buffer = {};
    playing_buffer.consumed = true;
}

void SinkStream::ProcessAudioIn(std::span<const s16> input_buffer, std::size_t num_frames)
{
    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};

    // If we're paused or going to shut down, we don't want to consume buffers as coretiming is
    // paused and we'll desync, so just return.
    if (system.IsPaused() || system.IsShuttingDown())
    {
        return;
    }

    while (frames_written < num_frames)
    {
        // If the playing buffer has been consumed or has no frames, we need a new one
        if (playing_buffer.consumed || playing_buffer.frames == 0)
        {
            if (!queue.try_dequeue(playing_buffer))
            {
                // If no buffer was available we've underrun, just push the samples and
                // continue.
                samples_buffer.Push(&input_buffer[frames_written * frame_size],
                                    (num_frames - frames_written) * frame_size);
                frames_written = num_frames;
                continue;
            }
            // Successfully dequeued a new buffer.
            queued_buffers--;
        }

        // Get the minimum frames available between the currently playing buffer, and the
        // amount we have left to fill
        size_t frames_available{std::min<u64>(playing_buffer.frames - playing_buffer.frames_played,
                                              num_frames - frames_written)};

        samples_buffer.Push(&input_buffer[frames_written * frame_size],
                            frames_available * frame_size);

        frames_written += frames_available;
        playing_buffer.frames_played += frames_available;

        // If that's all the frames in the current buffer, add its samples and mark it as
        // consumed
        if (playing_buffer.frames_played >= playing_buffer.frames)
        {
            playing_buffer.consumed = true;
        }
    }

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], &input_buffer[(frames_written - 1) * frame_size], frame_size_bytes);
    }

    // update sample counts fuer audio-ins
    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += frames_written;
    }
}

void SinkStream::ProcessAudioOutAndRender(std::span<s16> output_buffer, std::size_t num_frames)
{
    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};
    size_t actual_frames_written{0};
    if (num_frames == 0)
    {
        return;
    }

    // If we're paused or going to shut down, we don't want to consume buffers as coretiming is
    // paused and we'll desync, so just play silence.
    if (system.IsPaused() || system.IsShuttingDown())
    {
        if (system.IsShuttingDown())
        {
            {
                std::scoped_lock lk{release_mutex};
                queued_buffers.store(0);
            }
            release_cv.notify_one();
        }

        // Silence must cover the negotiated channel count, not a fixed six-sample array.
        for (size_t i = frames_written; i < num_frames; i++)
        {
            std::memset(&output_buffer[i * frame_size], 0, frame_size_bytes);
        }
        return;
    }

    while (frames_written < num_frames)
    {
        // If the playing buffer has been consumed or has no frames, we need a new one
        if (playing_buffer.consumed || playing_buffer.frames == 0)
        {
            if (!queue.try_dequeue(playing_buffer))
            {
                // If no buffer was available we've underrun, fill the remaining buffer with
                // the last written frame and continue.
                for (size_t i = frames_written; i < num_frames; i++)
                {
                    std::memcpy(&output_buffer[i * frame_size], &last_frame[0], frame_size_bytes);
                }
                frames_written = num_frames;
                continue;
            }
            // Successfully dequeued a new buffer.
            queued_buffers--;

            {
                std::unique_lock lk{release_mutex};
            }

            release_cv.notify_one();
        }

        // Get the minimum frames available between the currently playing buffer, and the
        // amount we have left to fill
        size_t frames_available{std::min<u64>(playing_buffer.frames - playing_buffer.frames_played,
                                              num_frames - frames_written)};

        s16 * out = &output_buffer[frames_written * frame_size];
        const u32 channels = playing_buffer.channels;
        // Only the frames this buffer stored are read from the ring, so that the samples of the
        // next buffer stay for it.
        const std::size_t stored_left =
            playing_buffer.stored_frames > playing_buffer.frames_played
                ? static_cast<std::size_t>(playing_buffer.stored_frames - playing_buffer.frames_played)
                : 0;
        const std::size_t stored = std::min(frames_available, stored_left);
        if (channels == frame_size)
        {
            const std::size_t wanted = frames_available * frame_size;
            const std::size_t read = samples_buffer.Pop(out, stored * frame_size);
            std::fill(out + read, out + wanted, s16{0});
        }
        else
        {
            const std::size_t chunk_frames = convert_buffer.size() / MaxChannels;
            for (std::size_t done = 0; done < frames_available; done += chunk_frames)
            {
                const std::size_t frames = std::min(chunk_frames, frames_available - done);
                const std::size_t from_ring = stored > done ? std::min(frames, stored - done) : 0;
                const std::size_t wanted = frames * channels;
                const std::size_t read =
                    from_ring > 0 ? samples_buffer.Pop(convert_buffer.data(), from_ring * channels) : 0;
                std::fill(convert_buffer.begin() + read, convert_buffer.begin() + wanted, s16{0});
                ConvertFrames(convert_buffer.data(), channels, out + done * frame_size,
                              static_cast<u32>(frame_size), frames);
            }
        }

        frames_written += frames_available;
        actual_frames_written += frames_available;
        playing_buffer.frames_played += frames_available;

        // If that's all the frames in the current buffer, add its samples and mark it as
        // consumed
        if (playing_buffer.frames_played >= playing_buffer.frames)
        {
            playing_buffer.consumed = true;
        }
    }

    std::memcpy(&last_frame[0], &output_buffer[(frames_written - 1) * frame_size],
                frame_size_bytes);

    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += actual_frames_written;
    }
}

u64 SinkStream::GetExpectedPlayedSampleCount()
{
    std::scoped_lock lk{sample_count_lock};
    auto cur_time{system.CoreTiming().GetGlobalTimeNs()};
    auto time_delta{cur_time - last_sample_count_update_time};
    auto exp_played_sample_count{min_played_sample_count +
                                 (TargetSampleRate * time_delta) / std::chrono::seconds{1}};

    // Add 15ms of latency in sample reporting to allow for some leeway in scheduler timings
    return std::min<u64>(exp_played_sample_count, max_played_sample_count) + TargetSampleCount * 3;
}

void SinkStream::WaitFreeSpace(std::stop_token stop_token)
{
    std::unique_lock lk{release_mutex};
    release_cv.wait_for(lk, std::chrono::milliseconds(5),
                        [this]() { return paused || queued_buffers < max_queue_size; });
    if (queued_buffers > max_queue_size + 3)
    {
        Common::CondvarWait(release_cv, lk, stop_token,
                            [this] { return paused || queued_buffers < max_queue_size; });
    }
}

void SinkStream::ResetDeviceChannels(u32 channels)
{
    device_channels.store(channels, std::memory_order_relaxed);
    last_frame.fill(0);
}

void SinkStream::SignalPause()
{
    {
        std::scoped_lock lk{release_mutex};
        paused = true;
    }
    release_cv.notify_one();
}

} // namespace AudioCore::Sink
