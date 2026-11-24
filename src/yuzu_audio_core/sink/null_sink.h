// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "yuzu_audio_core/common/common.h"
#include "yuzu_audio_core/sink/sink.h"
#include "yuzu_audio_core/sink/sink_stream.h"
#include "yuzu_common/thread.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {
        if (type != StreamType::In) {
            consumer = std::jthread([this](std::stop_token stop_token) { Consume(stop_token); });
        }
    }
    ~NullSinkStreamImpl() override {}

    void AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) override {
        // Nothing reads captured buffers back, so they must not pile up in the queue.
        if (type != StreamType::In) {
            SinkStream::AppendBuffer(buffer, samples);
        }
    }
    void Start(bool) override {
        paused = false;
    }
    void Stop() override {
        SignalPause();
    }

private:
    /// Plays queued output into nothing at the device rate. The audio renderer waits only for
    /// free queue space, so without it the renderer would run as fast as the game answers.
    void Consume(std::stop_token stop_token) {
        Common::SetCurrentThreadName("NullSinkStream");
        constexpr std::chrono::microseconds period{TargetSampleCount * 1'000'000ULL /
                                                   TargetSampleRate};
        std::vector<s16> discarded(TargetSampleCount * GetDeviceChannels());
        auto next = std::chrono::steady_clock::now();
        while (!stop_token.stop_requested()) {
            next += period;
            std::this_thread::sleep_until(next);
            if (!paused) {
                ProcessAudioOutAndRender(discarded, TargetSampleCount);
            }
            // Catch up after a late wake-up, but not with more than 20 blocks.
            next = std::max(next, std::chrono::steady_clock::now() - 20 * period);
        }
    }

    std::jthread consumer;
};

/**
 * A sink for when no audio out is wanted: output is played in real time into nothing and
 * capture returns silence.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels_, const std::string&,
                                  StreamType type) override {
        SinkStreamPtr& stream =
            sink_streams.emplace_back(std::make_unique<NullSinkStreamImpl>(system, type));
        stream->SetSystemChannels(system_channels_);
        stream->SetDeviceVolume(device_volume);
        return stream.get();
    }

    void CloseStream(SinkStream* stream) override {
        std::erase_if(sink_streams,
                      [stream](const SinkStreamPtr& s) { return s.get() == stream; });
    }
    void CloseStreams() override {
        sink_streams.clear();
    }
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : sink_streams) {
            stream->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {
        for (auto& stream : sink_streams) {
            stream->SetSystemVolume(volume);
        }
    }

private:
    std::vector<SinkStreamPtr> sink_streams{};
};

} // namespace AudioCore::Sink
