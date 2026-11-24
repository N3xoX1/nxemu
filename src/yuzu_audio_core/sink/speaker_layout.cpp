// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_audio_core/sink/speaker_layout.h"
#include "yuzu_common/logging/log.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
#include <string>
#include <vector>
#include <AudioToolbox/AudioFormat.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <cstring>
#include <utility>
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/string_util.h"
#elif defined(__linux__) && !defined(__ANDROID__) && __has_include(<pulse/pulseaudio.h>)
#define NXEMU_SPEAKERS_PULSE
#include <chrono>
#include <string>
#include <dlfcn.h>
#include <pulse/pulseaudio.h>
#endif

namespace AudioCore::Sink {

#if defined(__APPLE__) && TARGET_OS_OSX

namespace {

std::string ToUtf8(CFStringRef string) {
    if (const char* direct = CFStringGetCStringPtr(string, kCFStringEncodingUTF8)) {
        return direct;
    }
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(string, out.data(), capacity, kCFStringEncodingUTF8)) {
        return {};
    }
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

/// Reads a CFString property of an output device, empty if it has none.
std::string OutputString(AudioDeviceID device, AudioObjectPropertySelector selector,
                         AudioValueTranslation* translation = nullptr) {
    const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    CFStringRef string = nullptr;
    UInt32 size = translation ? sizeof(*translation) : sizeof(string);
    void* data = translation ? static_cast<void*>(translation) : static_cast<void*>(&string);
    if (translation) {
        translation->mOutputData = &string;
        translation->mOutputDataSize = sizeof(string);
    }
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, data) != noErr ||
        string == nullptr) {
        return {};
    }
    std::string out = ToUtf8(string);
    CFRelease(string);
    return out;
}

/// Whether an output device carries this name: SDL uses the device's name, cubeb the name of
/// its current data source when it has one, such as "MacBook Pro Speakers".
bool OutputNamed(AudioDeviceID device, std::string_view name) {
    const AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams,
                                             kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &size) != noErr ||
        size == 0) {
        return false;
    }
    if (OutputString(device, kAudioObjectPropertyName) == name) {
        return true;
    }
    const AudioObjectPropertyAddress source_address{kAudioDevicePropertyDataSource,
                                                    kAudioObjectPropertyScopeOutput,
                                                    kAudioObjectPropertyElementMain};
    UInt32 source = 0;
    size = sizeof(source);
    if (AudioObjectGetPropertyData(device, &source_address, 0, nullptr, &size, &source) !=
        noErr) {
        return false;
    }
    AudioValueTranslation translation{&source, sizeof(source), nullptr, 0};
    return OutputString(device, kAudioDevicePropertyDataSourceNameForIDCFString, &translation) ==
           name;
}

AudioDeviceID FindOutput(std::string_view name) {
    const AudioObjectPropertyAddress devices_address{kAudioHardwarePropertyDevices,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (!name.empty() && AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                        &devices_address, 0, nullptr,
                                                        &size) == noErr) {
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_address, 0, nullptr,
                                       &size, devices.data()) == noErr) {
            devices.resize(size / sizeof(AudioDeviceID));
            for (const AudioDeviceID device : devices) {
                if (OutputNamed(device, name)) {
                    return device;
                }
            }
        }
    }
    const AudioObjectPropertyAddress default_address{kAudioHardwarePropertyDefaultOutputDevice,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_address, 0, nullptr, &size,
                                   &device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

/// The positions a 5.1 stream reaches through a device layout. Core Audio's own channel map
/// covers layouts given as descriptions, standard tags or bitmaps, and matches what the output
/// unit plays. It fails on a layout with no known speaker, such as the unlabelled channels of
/// an interface nobody set up: no position then.
u32 MacSpeakersOfLayout(const AudioChannelLayout& layout) {
    AudioChannelLayout surround{};
    surround.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A;
    const AudioChannelLayout* layouts[2]{&surround, &layout};
    UInt32 map_size = 0;
    if (AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelMap, sizeof(layouts), layouts,
                                   &map_size) != noErr ||
        map_size < sizeof(SInt32)) {
        return 0;
    }
    // One entry per device channel: the 5.1 channel it plays, or -1.
    std::vector<SInt32> map(map_size / sizeof(SInt32));
    if (AudioFormatGetProperty(kAudioFormatProperty_ChannelMap, sizeof(layouts), layouts,
                               &map_size, map.data()) != noErr) {
        return 0;
    }
    map.resize(map_size / sizeof(SInt32));

    // MPEG_5_1_A: L R C LFE Ls Rs.
    constexpr u32 positions[6]{Speaker::FrontLeft,    Speaker::FrontRight, Speaker::FrontCenter,
                               Speaker::LowFrequency, Speaker::SideLeft,   Speaker::SideRight};
    u32 speakers = 0;
    for (const SInt32 channel : map) {
        if (channel >= 0 && channel < 6) {
            speakers |= positions[channel];
        }
    }
    return speakers;
}

/// The positions of the device's preferred layout, std::nullopt when it has none to read.
std::optional<u32> MappedSpeakers(AudioDeviceID device) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyPreferredChannelLayout,
                                             kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    constexpr UInt32 header_size = offsetof(AudioChannelLayout, mChannelDescriptions);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr ||
        size < header_size) {
        return std::nullopt;
    }
    // Aligned storage for the variable-length layout.
    std::vector<AudioChannelDescription> storage(size / sizeof(AudioChannelDescription) + 1);
    auto* layout = reinterpret_cast<AudioChannelLayout*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, layout) != noErr ||
        size < header_size ||
        (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions &&
         layout->mNumberChannelDescriptions >
             (size - header_size) / sizeof(AudioChannelDescription))) {
        return std::nullopt;
    }
    return MacSpeakersOfLayout(*layout);
}

} // namespace

struct SpeakerLayout::Impl {
    std::optional<u32> Query(std::string_view device_name) {
        const AudioDeviceID device = FindOutput(device_name);
        if (device == kAudioObjectUnknown) {
            return std::nullopt;
        }
        return MappedSpeakers(device);
    }
};

#elif defined(_WIN32)

namespace {

// Defined here rather than through initguid.h, as SDL does.
const PROPERTYKEY FriendlyNameKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY DeviceFormatKey{
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

struct ComRelease {
    void operator()(IUnknown* object) const {
        object->Release();
    }
};
template <typename T>
using ComPtr = std::unique_ptr<T, ComRelease>;

/// The positions of a WAVEFORMATEXTENSIBLE channel mask.
u32 SpeakersOfMask(DWORD mask) {
    constexpr std::pair<DWORD, u32> positions[]{
        {0x1, Speaker::FrontLeft},     {0x2, Speaker::FrontRight},
        {0x4, Speaker::FrontCenter},   {0x8, Speaker::LowFrequency},
        {0x10, Speaker::BackLeft},     {0x20, Speaker::BackRight},
        {0x200, Speaker::SideLeft},    {0x400, Speaker::SideRight},
    };
    u32 speakers = 0;
    for (const auto& [bit, position] : positions) {
        if (mask & bit) {
            speakers |= position;
        }
    }
    return speakers;
}

/// The active playback endpoint with this friendly name, the name cubeb and SDL list.
ComPtr<IMMDevice> FindEndpoint(IMMDeviceEnumerator& enumerator, std::string_view name) {
    IMMDeviceCollection* raw_collection = nullptr;
    if (FAILED(enumerator.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_collection))) {
        return nullptr;
    }
    const ComPtr<IMMDeviceCollection> collection{raw_collection};
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return nullptr;
    }
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* raw_device = nullptr;
        if (FAILED(collection->Item(i, &raw_device))) {
            continue;
        }
        ComPtr<IMMDevice> device{raw_device};
        IPropertyStore* raw_store = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_store))) {
            continue;
        }
        const ComPtr<IPropertyStore> store{raw_store};
        PROPVARIANT value;
        PropVariantInit(&value);
        const bool match = SUCCEEDED(store->GetValue(FriendlyNameKey, &value)) &&
                           value.vt == VT_LPWSTR && value.pwszVal != nullptr &&
                           Common::UTF16ToUTF8(std::wstring_view{value.pwszVal}) == name;
        PropVariantClear(&value);
        if (match) {
            return device;
        }
    }
    return nullptr;
}

} // namespace

struct SpeakerLayout::Impl {
    std::optional<u32> Query(std::string_view device_name) {
        // The audio monitor already joined the multithreaded apartment; the first check may
        // run on another thread. A thread in another apartment still gets the objects.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SCOPE_EXIT {
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
        };
        IMMDeviceEnumerator* raw_enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&raw_enumerator)))) {
            return std::nullopt;
        }
        const ComPtr<IMMDeviceEnumerator> enumerator{raw_enumerator};
        ComPtr<IMMDevice> device =
            device_name.empty() ? nullptr : FindEndpoint(*enumerator, device_name);
        if (!device) {
            // The default endpoint cubeb and SDL play on.
            IMMDevice* raw_device = nullptr;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &raw_device))) {
                return std::nullopt;
            }
            device.reset(raw_device);
        }
        IPropertyStore* raw_store = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_store))) {
            return std::nullopt;
        }
        const ComPtr<IPropertyStore> store{raw_store};

        // The format the audio engine plays to the device, with its speaker mask.
        PROPVARIANT value;
        PropVariantInit(&value);
        SCOPE_EXIT {
            PropVariantClear(&value);
        };
        if (FAILED(store->GetValue(DeviceFormatKey, &value)) || value.vt != VT_BLOB ||
            value.blob.pBlobData == nullptr || value.blob.cbSize < sizeof(WAVEFORMATEX)) {
            return std::nullopt;
        }
        WAVEFORMATEX format;
        std::memcpy(&format, value.blob.pBlobData, sizeof(format));
        if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            WAVEFORMATEXTENSIBLE extensible;
            std::memcpy(&extensible, value.blob.pBlobData, sizeof(extensible));
            return SpeakersOfMask(extensible.dwChannelMask);
        }
        // Without a mask, only a stereo format has known positions.
        if (format.nChannels <= 2) {
            return Speaker::FrontLeft | Speaker::FrontRight;
        }
        return std::nullopt;
    }
};

#elif defined(NXEMU_SPEAKERS_PULSE)

#define NXEMU_PULSE_FUNCTIONS(X)                                                                   \
    X(pa_mainloop_new)                                                                             \
    X(pa_mainloop_free)                                                                            \
    X(pa_mainloop_get_api)                                                                         \
    X(pa_mainloop_prepare)                                                                         \
    X(pa_mainloop_poll)                                                                            \
    X(pa_mainloop_dispatch)                                                                        \
    X(pa_context_new)                                                                              \
    X(pa_context_connect)                                                                          \
    X(pa_context_disconnect)                                                                       \
    X(pa_context_unref)                                                                            \
    X(pa_context_get_state)                                                                        \
    X(pa_context_get_server_info)                                                                  \
    X(pa_context_get_sink_info_list)                                                               \
    X(pa_operation_get_state)                                                                      \
    X(pa_operation_cancel)                                                                         \
    X(pa_operation_unref)

namespace {

/// The positions of a PulseAudio channel map. PipeWire serves the same protocol; the AUX
/// channels of a pro-audio profile have no position.
u32 SpeakersOfMap(const pa_channel_map& map) {
    u32 speakers = 0;
    for (u8 i = 0; i < map.channels && i < PA_CHANNELS_MAX; ++i) {
        switch (map.map[i]) {
        case PA_CHANNEL_POSITION_FRONT_LEFT:
            speakers |= Speaker::FrontLeft;
            break;
        case PA_CHANNEL_POSITION_FRONT_RIGHT:
            speakers |= Speaker::FrontRight;
            break;
        case PA_CHANNEL_POSITION_FRONT_CENTER:
            speakers |= Speaker::FrontCenter;
            break;
        case PA_CHANNEL_POSITION_LFE:
            speakers |= Speaker::LowFrequency;
            break;
        case PA_CHANNEL_POSITION_REAR_LEFT:
            speakers |= Speaker::BackLeft;
            break;
        case PA_CHANNEL_POSITION_REAR_RIGHT:
            speakers |= Speaker::BackRight;
            break;
        case PA_CHANNEL_POSITION_SIDE_LEFT:
            speakers |= Speaker::SideLeft;
            break;
        case PA_CHANNEL_POSITION_SIDE_RIGHT:
            speakers |= Speaker::SideRight;
            break;
        default:
            break;
        }
    }
    return speakers;
}

struct SinkLookup {
    std::string_view name;
    std::string default_sink{};
    std::optional<u32> named{};
    std::optional<u32> by_default{};
};

} // namespace

/// A client of the sound server, loaded at run time as cubeb does, so that a system without
/// libpulse still runs. Connected once and kept, with a bounded wait for every answer.
struct SpeakerLayout::Impl {
    ~Impl() {
        Disconnect();
        if (library) {
            dlclose(library);
        }
    }

    std::optional<u32> Query(std::string_view device_name) {
        if (!Load() || !Connect()) {
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        SinkLookup lookup{.name = device_name};
        const bool answered =
            Wait(pa_context_get_server_info(context, &OnServerInfo, &lookup), deadline) &&
            Wait(pa_context_get_sink_info_list(context, &OnSink, &lookup), deadline);
        if (!answered) {
            // A server that stopped answering is connected to again at the next check.
            Disconnect();
            return std::nullopt;
        }
        return lookup.named ? lookup.named : lookup.by_default;
    }

private:
    static void OnServerInfo(pa_context*, const pa_server_info* info, void* userdata) {
        if (info != nullptr && info->default_sink_name != nullptr) {
            static_cast<SinkLookup*>(userdata)->default_sink = info->default_sink_name;
        }
    }

    /// cubeb and SDL list a sink by its description.
    static void OnSink(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
        if (eol != 0 || info == nullptr) {
            return;
        }
        auto& lookup = *static_cast<SinkLookup*>(userdata);
        const u32 speakers = SpeakersOfMap(info->channel_map);
        if (!lookup.name.empty() && !lookup.named &&
            ((info->description != nullptr && lookup.name == info->description) ||
             (info->name != nullptr && lookup.name == info->name))) {
            lookup.named = speakers;
        }
        if (info->name != nullptr && lookup.default_sink == info->name) {
            lookup.by_default = speakers;
        }
    }

    bool Load() {
        if (library) {
            return true;
        }
        if (load_failed) {
            return false;
        }
        library = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        load_failed = library == nullptr;
#define NXEMU_LOAD(name)                                                                           \
    if (library && !(name = reinterpret_cast<decltype(name)>(dlsym(library, #name)))) {            \
        dlclose(library);                                                                          \
        library = nullptr;                                                                         \
        load_failed = true;                                                                        \
    }
        NXEMU_PULSE_FUNCTIONS(NXEMU_LOAD)
#undef NXEMU_LOAD
        if (load_failed) {
            LOG_INFO(Audio_Sink, "libpulse is not available: speaker layouts are unknown");
        }
        return !load_failed;
    }

    bool Connect() {
        if (context && pa_context_get_state(context) == PA_CONTEXT_READY) {
            return true;
        }
        Disconnect();
        loop = pa_mainloop_new();
        if (!loop) {
            return false;
        }
        context = pa_context_new(pa_mainloop_get_api(loop), "NxEmu speaker layout");
        if (!context ||
            pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            Disconnect();
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        for (;;) {
            const pa_context_state_t state = pa_context_get_state(context);
            if (state == PA_CONTEXT_READY) {
                return true;
            }
            if (!PA_CONTEXT_IS_GOOD(state) || !Iterate(deadline)) {
                Disconnect();
                return false;
            }
        }
    }

    void Disconnect() {
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            context = nullptr;
        }
        if (loop) {
            pa_mainloop_free(loop);
            loop = nullptr;
        }
    }

    /// Runs the loop once, waiting no later than the deadline.
    bool Iterate(std::chrono::steady_clock::time_point deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return false;
        }
        return pa_mainloop_prepare(loop, static_cast<int>(left.count())) >= 0 &&
               pa_mainloop_poll(loop) >= 0 && pa_mainloop_dispatch(loop) >= 0;
    }

    /// Waits for an operation, cancelling it at the deadline so that its callback, which
    /// points to the caller's stack, never runs later.
    bool Wait(pa_operation* operation, std::chrono::steady_clock::time_point deadline) {
        if (!operation) {
            return false;
        }
        bool done = true;
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
            if (!Iterate(deadline)) {
                pa_operation_cancel(operation);
                done = false;
                break;
            }
        }
        done = done && pa_operation_get_state(operation) == PA_OPERATION_DONE;
        pa_operation_unref(operation);
        return done;
    }

    void* library{};
    bool load_failed{};
#define NXEMU_DECLARE(name) decltype(&::name) name{};
    NXEMU_PULSE_FUNCTIONS(NXEMU_DECLARE)
#undef NXEMU_DECLARE
    pa_mainloop* loop{};
    pa_context* context{};
};

#undef NXEMU_PULSE_FUNCTIONS

#else

/// No speaker layout to read: outputs play stereo.
struct SpeakerLayout::Impl {
    std::optional<u32> Query(std::string_view) {
        return std::nullopt;
    }
};

#endif

SpeakerLayout::SpeakerLayout() : impl{std::make_unique<Impl>()} {}

SpeakerLayout::~SpeakerLayout() = default;

std::optional<u32> SpeakerLayout::QuerySpeakers(std::string_view device_name) {
    return impl->Query(device_name);
}

u32 SpeakerLayout::HostChannels(u32 channels, std::string_view device_name) {
    // Fewer than six channels play stereo whatever the speakers, so they are not read.
    const std::optional<u32> speakers =
        channels >= 6 ? QuerySpeakers(device_name) : std::nullopt;
    const u32 host = SurroundPolicy(channels, speakers);
    if (channels >= 6 && (channels != logged_channels || speakers != logged_speakers)) {
        if (host == 6) {
            LOG_INFO(Audio_Sink, "Output has 5.1 speakers (positions {:#x})", *speakers);
        } else if (speakers) {
            LOG_INFO(Audio_Sink,
                     "Output has {} channels but no 5.1 speakers (positions {:#x}), playing "
                     "stereo",
                     channels, *speakers);
        } else {
            LOG_INFO(Audio_Sink,
                     "Output has {} channels but reports no speaker layout, playing stereo",
                     channels);
        }
    }
    logged_channels = channels;
    logged_speakers = speakers;
    return host;
}

} // namespace AudioCore::Sink
