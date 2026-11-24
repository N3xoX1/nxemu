// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <SDL3/SDL.h>

#include "yuzu_audio_core/common/common.h"
#include "yuzu_audio_core/sink/sdl_sink.h"
#include "yuzu_audio_core/sink/sink_stream.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "core/core.h"

namespace AudioCore::Sink {
namespace {
/// Finds an audio device by name. An empty or unknown name gives the default device.
SDL_AudioDeviceID FindAudioDevice(const std::string& name, bool capture) {
    const SDL_AudioDeviceID default_device =
        capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (name.empty()) {
        return default_device;
    }
    int count = 0;
    SDL_AudioDeviceID* devices =
        capture ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
    if (devices == nullptr) {
        return default_device;
    }
    SDL_AudioDeviceID found = default_device;
    for (int i = 0; i < count; ++i) {
        const char* device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name != nullptr && name == device_name) {
            found = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return found;
}

bool InitAudio() {
    if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return true;
    }
    LOG_CRITICAL(Audio_Sink, "SDL_InitSubSystem audio failed: {}", SDL_GetError());
    return false;
}
} // Anonymous namespace

/**
 * SDL sink stream, responsible for sinking samples to hardware.
 */
class SDLSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param device_channels_ - Number of channels to open the hardware stream with.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device    - Name of the output device to use for this stream.
     * @param input_device     - Name of the input device to use for this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     */
    SDLSinkStream(u32 device_channels_, u32 system_channels_, const std::string& output_device,
                  const std::string& input_device, StreamType type_, Core::System& system_)
        : SinkStream{system_, type_} {
        system_channels = system_channels_;
        // Recording is stereo; only playback follows the output layout.
        device_channels = type == StreamType::In ? 2 : device_channels_;
        device_name = type == StreamType::In ? input_device : output_device;
        std::scoped_lock lock{backend_mutex};
        OpenBackend();
    }

    /**
     * Destroy the sink stream.
     */
    ~SDLSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing SDL stream {}", name);
        Finalize();
    }

    /**
     * Finalize the sink stream.
     */
    void Finalize() override {
        Stop();
        std::scoped_lock lock{backend_mutex};
        CloseBackend();
    }

    /**
     * Start the sink stream.
     *
     * @param resume - Set to true if this is resuming the stream a previously-active stream.
     *                 Default false.
     */
    void Start(bool resume = false) override {
        std::scoped_lock lock{backend_mutex};
        if (stream == nullptr || !paused) {
            return;
        }

        paused = false;
        SDL_ResumeAudioStreamDevice(stream);
    }

    /**
     * Stop the sink stream.
     */
    void Stop() override {
        std::scoped_lock lock{backend_mutex};
        if (stream == nullptr || paused) {
            return;
        }
        SignalPause();
        SDL_PauseAudioStreamDevice(stream);
    }

    u32 SetDeviceChannels(u32 channels) override {
        if (type == StreamType::In) {
            return channels;
        }
        std::scoped_lock lock{backend_mutex};
        if (channels == GetDeviceChannels() && stream != nullptr) {
            return channels;
        }
        // Queued audio stays in the game's layout, so it plays on in the new one.
        CloseBackend();
        ResetDeviceChannels(channels);
        OpenBackend();
        if (stream != nullptr && !paused) {
            SDL_ResumeAudioStreamDevice(stream);
        }
        return stream != nullptr ? GetDeviceChannels() : 0;
    }

    /// Closes the device, to read its own layout or to open it again in another one.
    void CloseDevice() {
        if (type == StreamType::In) {
            return;
        }
        std::scoped_lock lock{backend_mutex};
        CloseBackend();
    }

private:
    /// Opens the device, paused, with device_channels, or in stereo when the device refuses
    /// them. Needs backend_mutex.
    void OpenBackend() {
        if (TryOpenBackend() || GetDeviceChannels() == 2) {
            return;
        }
        LOG_WARNING(Audio_Sink, "SDL refused {} channels, playing stereo", GetDeviceChannels());
        ResetDeviceChannels(2);
        TryOpenBackend();
    }

    /// Opens the device, paused, with device_channels. Needs backend_mutex.
    bool TryOpenBackend() {
        // SDL converts this format to the device's, if they differ.
        SDL_AudioSpec spec{};
        spec.freq = TargetSampleRate;
        spec.channels = static_cast<int>(GetDeviceChannels());
        spec.format = SDL_AUDIO_S16;

        // Keep the device period SDL 2 was asked for. The hint is read when a device opens.
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
                    std::to_string(TargetSampleCount * 2).c_str());
        stream = SDL_OpenAudioDeviceStream(FindAudioDevice(device_name, type == StreamType::In),
                                           &spec, &SDLSinkStream::DataCallback, this);
        if (stream == nullptr) {
            LOG_CRITICAL(Audio_Sink, "Error opening SDL audio device: {}", SDL_GetError());
            return false;
        }

        SDL_AudioSpec obtained{};
        int sample_frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &obtained, &sample_frames);
        LOG_INFO(Service_Audio,
                 "Opening SDL stream {} with: rate {} channels {} (system channels {}) "
                 " samples {}",
                 SDL_GetAudioStreamDevice(stream), obtained.freq, obtained.channels,
                 GetSystemChannels(), sample_frames);
        return true;
    }

    /// Destroys the stream; its callback has returned once this does. Needs backend_mutex.
    void CloseBackend() {
        if (stream == nullptr) {
            return;
        }
        // Also closes the device the stream opened.
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }

    /**
     * Main callback from SDL. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param userdata          - Custom data pointer passed along, points to a SDLSinkStream.
     * @param stream            - Stream to put samples in, or to get them from.
     * @param additional_amount - Bytes the device needs now, or that it recorded.
     * @param total_amount      - Bytes the device needs, including those still queued.
     */
    static void SDLCALL DataCallback(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int total_amount) {
        auto* impl = static_cast<SDLSinkStream*>(userdata);

        if (!impl || additional_amount <= 0) {
            return;
        }

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t frame_bytes = frame_size * sizeof(s16);
        // Playback rounds up to whole frames; recording takes only the whole frames available.
        const std::size_t num_frames = impl->type == StreamType::In
                                           ? additional_amount / frame_bytes
                                           : (additional_amount + frame_bytes - 1) / frame_bytes;
        if (num_frames == 0) {
            return;
        }
        std::vector<s16>& buffer = impl->callback_buffer;
        buffer.resize(num_frames * frame_size);
        const int bytes = static_cast<int>(num_frames * frame_bytes);

        if (impl->type == StreamType::In) {
            const int read = SDL_GetAudioStreamData(stream, buffer.data(), bytes);
            if (read <= 0) {
                return;
            }
            const std::size_t read_frames = static_cast<std::size_t>(read) / frame_bytes;
            impl->ProcessAudioIn(std::span<const s16>{buffer.data(), read_frames * frame_size},
                                 read_frames);
        } else {
            impl->ProcessAudioOutAndRender(std::span<s16>{buffer}, num_frames);
            SDL_PutAudioStreamData(stream, buffer.data(), bytes);
        }
    }

    /// Name of the device this stream plays on or records from, empty for the default
    std::string device_name;
    /// SDL stream bound to the opened input/output device
    SDL_AudioStream* stream{};
    /// Samples passed between the stream and the audio system, used only by the callback
    std::vector<s16> callback_buffer;
    /// Guards stream against a reopen from the output monitor
    std::mutex backend_mutex;
};

namespace {
std::string DefaultOutputName() {
    const char* name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    return name != nullptr ? name : "";
}

// Runs on the thread that pumps events; only flags the change for the output monitor.
bool SDLCALL WatchAudioFormat(void* format_changed, SDL_Event* event) {
    if (event->type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED && !event->adevice.recording) {
        static_cast<std::atomic<bool>*>(format_changed)->store(true, std::memory_order_relaxed);
    }
    return true;
}
} // Anonymous namespace

SDLSink::SDLSink(std::string_view target_device_name) {
    if (!InitAudio()) {
        return;
    }

    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        output_device = target_device_name;
    } else {
        output_device.clear();
        default_output = DefaultOutputName();
    }
    output_channels = ProbeOutputChannels();
    // A device set up anew (stereo to 5.1, say) keeps its name: SDL reports it with an event.
    SDL_AddEventWatch(&WatchAudioFormat, &format_changed);
}

SDLSink::~SDLSink() {
    SDL_RemoveEventWatch(&WatchAudioFormat, &format_changed);
}

SinkStream* SDLSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                       const std::string&, StreamType type) {
    std::scoped_lock lock{streams_mutex};
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<SDLSinkStream>(
        GetDeviceChannels(), system_channels_, output_device, input_device, type, system));
    return stream.get();
}

void SDLSink::CloseStream(SinkStream* stream) {
    SinkStreamPtr closed;
    {
        std::scoped_lock lock{streams_mutex};
        const auto it = std::find_if(sink_streams.begin(), sink_streams.end(),
                                     [stream](const SinkStreamPtr& s) { return s.get() == stream; });
        if (it == sink_streams.end()) {
            return;
        }
        closed = std::move(*it);
        sink_streams.erase(it);
    }
    // Destroyed outside the lock: it waits for its audio callback.
}

void SDLSink::CloseStreams() {
    std::vector<SinkStreamPtr> closed;
    std::scoped_lock lock{streams_mutex};
    closed.swap(sink_streams);
}

f32 SDLSink::GetDeviceVolume() const {
    std::scoped_lock lock{streams_mutex};
    if (sink_streams.empty()) {
        return 1.0f;
    }

    return sink_streams[0]->GetDeviceVolume();
}

void SDLSink::SetDeviceVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void SDLSink::SetSystemVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

Sink::PlayingChannels SDLSink::GetPlayingChannels() const {
    std::scoped_lock lock{streams_mutex};
    return PlayingChannelsOf(sink_streams);
}

u32 SDLSink::ProbeOutputChannels() const {
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(FindAudioDevice(output_device, false), &spec, nullptr)) {
        return 2;
    }
    return static_cast<u32>(spec.channels);
}

bool SDLSink::OutputDeviceChanged() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        return false;
    }
    const bool format_changed_now = format_changed.exchange(false, std::memory_order_relaxed);
    if (output_device.empty()) {
        std::string name = DefaultOutputName();
        if (name != default_output) {
            LOG_INFO(Audio_Sink, "Default output is now {}", name);
            default_output = std::move(name);
            // SDL moved the streams to the new default with their own layout, which is then
            // all it reports. Close them to read the device's.
            std::scoped_lock lock{streams_mutex};
            for (auto& stream : sink_streams) {
                static_cast<SDLSinkStream*>(stream.get())->CloseDevice();
            }
            output_channels = ProbeOutputChannels();
            return true;
        }
    }
    if (format_changed_now) {
        // The same device set up anew: SDL already holds its new format, open or not.
        output_channels = ProbeOutputChannels();
    }
    return false;
}

u32 SDLSink::QueryHostChannels() {
    return output_channels;
}

std::string SDLSink::OutputDeviceName() const {
    return output_device;
}

u32 SDLSink::ReopenPlayback(u32 channels) {
    std::scoped_lock lock{streams_mutex};
    // The streams share one physical device, which keeps the format it was opened with and
    // converts the others to it (a 5.1 stream on a device left in stereo plays 10 dB lower).
    // Close them all first, so that the device opens again in the new layout.
    const auto close_all = [this] {
        for (auto& stream : sink_streams) {
            static_cast<SDLSinkStream*>(stream.get())->CloseDevice();
        }
    };
    close_all();
    const u32 opened = ReopenStreams(sink_streams, channels);
    if (opened < channels) {
        // A refused layout: the streams were reopened one by one, so once more all together.
        close_all();
        ReopenStreams(sink_streams, opened);
    }
    return opened;
}

std::vector<std::string> ListSDLSinkDevices(bool capture) {
    std::vector<std::string> device_list;

    if (!InitAudio()) {
        return {};
    }

    int device_count = 0;
    SDL_AudioDeviceID* devices = capture ? SDL_GetAudioRecordingDevices(&device_count)
                                         : SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == nullptr) {
        return {};
    }
    for (int i = 0; i < device_count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            device_list.emplace_back(name);
        }
    }
    SDL_free(devices);

    return device_list;
}

bool IsSDLSuitable() {
#if !defined(HAVE_SDL)
    return false;
#else
    // Check SDL can init
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR(Audio_Sink, "SDL failed to init, it is not suitable. Error: {}",
                  SDL_GetError());
        return false;
    }

    // We can set any latency frequency we want with SDL, so no need to check that.

    // Check we can open a device with standard parameters
    SDL_AudioSpec spec{};
    spec.freq = TargetSampleRate;
    spec.channels = 2;
    spec.format = SDL_AUDIO_S16;

    const SDL_AudioDeviceID device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);

    if (device == 0) {
        LOG_ERROR(Audio_Sink, "SDL failed to open a device, it is not suitable. Error: {}",
                  SDL_GetError());
        return false;
    }

    SDL_CloseAudioDevice(device);
    return true;
#endif
}

} // namespace AudioCore::Sink
