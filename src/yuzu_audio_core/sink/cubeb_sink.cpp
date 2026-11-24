// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

#include "yuzu_audio_core/common/common.h"
#include "yuzu_audio_core/sink/cubeb_sink.h"
#include "yuzu_audio_core/sink/sink_stream.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#undef CreateEvent
#endif

namespace AudioCore::Sink {
/**
 * Cubeb sink stream, responsible for sinking samples to hardware.
 */
class CubebSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param ctx_             - Cubeb context to create this stream with.
     * @param device_channels_ - Number of channels to open the hardware stream with.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device_   - Cubeb output device id.
     * @param input_device_    - Cubeb input device id.
     * @param name_            - Name of this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     */
    CubebSinkStream(cubeb* ctx_, u32 device_channels_, u32 system_channels_,
                    cubeb_devid output_device_, cubeb_devid input_device_,
                    const std::string& name_, StreamType type_, Core::System& system_)
        : SinkStream(system_, type_), ctx{ctx_}, output_device{output_device_},
          input_device{input_device_} {
#ifdef _WIN32
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        name = name_;
        // Recording is stereo; only playback follows the output layout.
        device_channels = type == StreamType::In ? 2 : device_channels_;
        system_channels = system_channels_;
        std::scoped_lock lock{backend_mutex};
        OpenBackend();
    }

    /**
     * Destroy the sink stream.
     */
    ~CubebSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing cubeb stream {}", name);
        Finalize();
#ifdef _WIN32
        CoUninitialize();
#endif
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
        if (!stream_backend || !paused) {
            return;
        }

        paused = false;
        if (cubeb_stream_start(stream_backend) != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error starting cubeb stream");
        }
    }

    /**
     * Stop the sink stream.
     */
    void Stop() override {
        std::scoped_lock lock{backend_mutex};
        if (!stream_backend || paused) {
            return;
        }

        SignalPause();
        if (cubeb_stream_stop(stream_backend) != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error stopping cubeb stream");
        }
    }

    u32 SetDeviceChannels(u32 channels) override {
        if (type == StreamType::In) {
            return channels;
        }
        std::scoped_lock lock{backend_mutex};
        if (channels == GetDeviceChannels() && stream_backend) {
            return channels;
        }
        // Queued audio stays in the game's layout, so it plays on in the new one.
        CloseBackend();
        ResetDeviceChannels(channels);
        OpenBackend();
        if (stream_backend && !paused && cubeb_stream_start(stream_backend) != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error restarting cubeb stream");
        }
        return stream_backend ? GetDeviceChannels() : 0;
    }

private:
    /// Opens the hardware stream with device_channels, or in stereo when the device refuses
    /// them. Needs backend_mutex.
    void OpenBackend() {
        if (!ctx || TryOpenBackend() || GetDeviceChannels() == 2) {
            return;
        }
        LOG_WARNING(Audio_Sink, "Cubeb stream {} refused {} channels, playing stereo", name,
                    GetDeviceChannels());
        ResetDeviceChannels(2);
        TryOpenBackend();
    }

    /// Opens the hardware stream with device_channels. Needs backend_mutex.
    bool TryOpenBackend() {
        cubeb_stream_params params{};
        params.rate = TargetSampleRate;
        params.channels = GetDeviceChannels();
        params.format = CUBEB_SAMPLE_S16LE;
        params.prefs = CUBEB_STREAM_PREF_NONE;
        // The Switch's 5.1 order: front left, front right, center, LFE, surround left, right.
        params.layout = params.channels == 6 ? CUBEB_LAYOUT_3F2_LFE : CUBEB_LAYOUT_STEREO;

        u32 minimum_latency{0};
        const auto latency_error = cubeb_get_min_latency(ctx, &params, &minimum_latency);
        if (latency_error != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
            minimum_latency = TargetSampleCount * 2;
        }

        minimum_latency = std::max(minimum_latency, TargetSampleCount * 2);

        LOG_INFO(Service_Audio,
                 "Opening cubeb stream {} type {} with: rate {} channels {} (system channels {}) "
                 "latency {}",
                 name, type, params.rate, params.channels, GetSystemChannels(), minimum_latency);

        auto init_error{0};
        if (type == StreamType::In) {
            init_error = cubeb_stream_init(ctx, &stream_backend, name.c_str(), input_device,
                                           &params, nullptr, nullptr, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        } else {
            init_error = cubeb_stream_init(ctx, &stream_backend, name.c_str(), nullptr, nullptr,
                                           output_device, &params, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        }

        if (init_error != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error initializing cubeb stream, error: {}", init_error);
            stream_backend = nullptr;
            return false;
        }
        return true;
    }

    /// Destroys the hardware stream; its callback has returned once this does. Needs backend_mutex.
    void CloseBackend() {
        if (!stream_backend) {
            return;
        }
        cubeb_stream_stop(stream_backend);
        cubeb_stream_destroy(stream_backend);
        stream_backend = nullptr;
    }

    /**
     * Main callback from Cubeb. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param in_buff     - Input buffer to be used if the stream is an input type.
     * @param out_buff    - Output buffer to be used if the stream is an output type.
     * @param num_frames_ - Number of frames of audio in the buffers. Note: Not number of samples.
     */
    static long DataCallback([[maybe_unused]] cubeb_stream* stream, void* user_data,
                             [[maybe_unused]] const void* in_buff, void* out_buff,
                             long num_frames_) {
        auto* impl = static_cast<CubebSinkStream*>(user_data);
        if (!impl) {
            return -1;
        }

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t num_frames{static_cast<size_t>(num_frames_)};

        if (impl->type == StreamType::In) {
            std::span<const s16> input_buffer{reinterpret_cast<const s16*>(in_buff),
                                              num_frames * frame_size};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            std::span<s16> output_buffer{reinterpret_cast<s16*>(out_buff), num_frames * frame_size};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
        }

        return num_frames_;
    }

    /**
     * Cubeb callback for if a device state changes. Unused currently.
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param state       - New state of the device.
     */
    static void StateCallback(cubeb_stream*, void*, cubeb_state) {}

    /// Main Cubeb context
    cubeb* ctx{};
    /// Cubeb id of the output device, null for the system default
    cubeb_devid output_device{};
    /// Cubeb id of the input device, null for the system default
    cubeb_devid input_device{};
    /// Cubeb stream backend
    cubeb_stream* stream_backend{};
    /// Guards stream_backend against a reopen from the output monitor
    std::mutex backend_mutex;
};

CubebSink::CubebSink(std::string_view target_device_name) {
    // Cubeb requires COM to be initialized on the thread calling cubeb_init on Windows
#ifdef _WIN32
    com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "NxEmu", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return;
    }

    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        cubeb_device_collection collection;
        if (cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection) != CUBEB_OK) {
            LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
        } else {
            const auto collection_end{collection.device + collection.count};
            const auto device{
                std::find_if(collection.device, collection_end, [&](const cubeb_device_info& info) {
                    return info.friendly_name != nullptr &&
                           target_device_name == std::string(info.friendly_name);
                })};
            if (device != collection_end) {
                output_device = device->devid;
                chosen_device = target_device_name;
                chosen_device_channels = device->max_channels;
            }
            cubeb_device_collection_destroy(ctx, &collection);
        }
    }
}

CubebSink::~CubebSink() {
    if (!ctx) {
        return;
    }

    for (auto& sink_stream : sink_streams) {
        sink_stream.reset();
    }

    cubeb_destroy(ctx);

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif
}

SinkStream* CubebSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                         const std::string& name, StreamType type) {
    std::scoped_lock lock{streams_mutex};
    SinkStreamPtr& stream = sink_streams.emplace_back(
        std::make_unique<CubebSinkStream>(ctx, GetDeviceChannels(), system_channels_,
                                          output_device, input_device, name, type, system));

    return stream.get();
}

void CubebSink::CloseStream(SinkStream* stream) {
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

void CubebSink::CloseStreams() {
    std::vector<SinkStreamPtr> closed;
    std::scoped_lock lock{streams_mutex};
    closed.swap(sink_streams);
}

f32 CubebSink::GetDeviceVolume() const {
    std::scoped_lock lock{streams_mutex};
    if (sink_streams.empty()) {
        return 1.0f;
    }

    return sink_streams[0]->GetDeviceVolume();
}

void CubebSink::SetDeviceVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void CubebSink::SetSystemVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

Sink::PlayingChannels CubebSink::GetPlayingChannels() const {
    std::scoped_lock lock{streams_mutex};
    return PlayingChannelsOf(sink_streams);
}

u32 CubebSink::QueryHostChannels() {
    if (!ctx) {
        return 2;
    }
    if (!chosen_device.empty()) {
        // Read again each time, as the device's layout can be set up anew in the system.
        cubeb_device_collection collection;
        if (cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection) == CUBEB_OK) {
            const auto end{collection.device + collection.count};
            const auto device{std::find_if(collection.device, end, [&](const cubeb_device_info& info) {
                return info.friendly_name != nullptr && chosen_device == info.friendly_name;
            })};
            if (device != end) {
                chosen_device_channels = device->max_channels;
            }
            cubeb_device_collection_destroy(ctx, &collection);
        }
        return chosen_device_channels;
    }
    // The layout of the current default output, as set up in the system's sound settings.
    u32 channels{2};
    if (cubeb_get_max_channel_count(ctx, &channels) != CUBEB_OK) {
        return 2;
    }
    return channels;
}

std::string CubebSink::OutputDeviceName() const {
    return chosen_device;
}

u32 CubebSink::ReopenPlayback(u32 channels) {
    std::scoped_lock lock{streams_mutex};
    return ReopenStreams(sink_streams, channels);
}

std::vector<std::string> ListCubebSinkDevices(bool capture) {
    std::vector<std::string> device_list;
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "yuzu Device Enumerator", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return {};
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    auto type{capture ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT};
    cubeb_device_collection collection;
    if (cubeb_enumerate_devices(ctx, type, &collection) != CUBEB_OK) {
        LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
    } else {
        for (std::size_t i = 0; i < collection.count; i++) {
            const cubeb_device_info& device = collection.device[i];
            if (device.friendly_name && device.friendly_name[0] != '\0' &&
                device.state == CUBEB_DEVICE_STATE_ENABLED) {
                device_list.emplace_back(device.friendly_name);
            }
        }
        cubeb_device_collection_destroy(ctx, &collection);
    }

    cubeb_destroy(ctx);
    return device_list;
}

namespace {
static long TmpDataCallback(cubeb_stream*, void*, const void*, void*, long) {
    return TargetSampleCount;
}
static void TmpStateCallback(cubeb_stream*, void*, cubeb_state) {}
} // namespace

bool IsCubebSuitable() {
#if !defined(HAVE_CUBEB)
    return false;
#else
    cubeb* ctx{nullptr};

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "yuzu Latency Getter", nullptr) != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb failed to init, it is not suitable.");
        return false;
    }

    SCOPE_EXIT {
        cubeb_destroy(ctx);
    };

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb could not get min latency, it is not suitable.");
        return false;
    }
    latency = std::max(latency, TargetSampleCount * 2);

    // Test opening a device with standard parameters
    cubeb_devid output_device{0};
    cubeb_devid input_device{0};
    std::string name{"Yuzu test"};
    cubeb_stream* stream{nullptr};

    if (cubeb_stream_init(ctx, &stream, name.c_str(), input_device, nullptr, output_device, &params,
                          latency, &TmpDataCallback, &TmpStateCallback, nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Cubeb could not open a device, it is not suitable.");
        return false;
    }

    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    return true;
#endif
}

} // namespace AudioCore::Sink
