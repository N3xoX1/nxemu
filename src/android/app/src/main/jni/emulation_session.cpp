#include "emulation_session.h"

#include <android/log.h>
#include <android/native_window.h>
#include <cstdio>
#include <vector>

#include <common/base64.h>
#include <common/json.h>
#include <yuzu_common/android/java_bridge.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/video.h>

namespace
{
    constexpr const char * kLogTag = "NxEmu";
    constexpr uint32_t kMaxArtworkBytes = 16 * 1024 * 1024;

    std::vector<uint8_t> ReadRomImage(IRomInfo * rom, LoaderResultStatus (IRomInfo::*reader)(uint8_t *, uint32_t *))
    {
        if (rom == nullptr)
        {
            return {};
        }
        uint32_t size = 0;
        LoaderResultStatus status = (rom->*reader)(nullptr, &size);
        if (status != LoaderResultStatus::Success || size == 0 || size >= kMaxArtworkBytes)
        {
            return {};
        }
        std::vector<uint8_t> data(size);
        status = (rom->*reader)(data.data(), &size);
        if (status != LoaderResultStatus::Success || size == 0)
        {
            return {};
        }
        data.resize(size);
        return data;
    }

    std::string EncodeRomImage(IRomInfo * rom, LoaderResultStatus (IRomInfo::*reader)(uint8_t *, uint32_t *))
    {
        const std::vector<uint8_t> data = ReadRomImage(rom, reader);
        return data.empty() ? std::string() : base64_encode(data.data(), data.size());
    }

    std::string ReadRomTitle(IRomInfo * rom)
    {
        if (rom == nullptr)
        {
            return {};
        }
        std::string title;
        uint32_t title_sz = 0;
        LoaderResultStatus title_res = rom->ReadTitle(nullptr, &title_sz);
        if (title_res == LoaderResultStatus::Success && title_sz > 0 && title_sz < 1024 * 1024)
        {
            title.resize(title_sz);
            title_res = rom->ReadTitle(title.data(), &title_sz);
            if (title_res != LoaderResultStatus::Success)
            {
                title.clear();
            }
        }
        if (const auto nul = title.find('\0'); nul != std::string::npos)
        {
            title.resize(nul);
        }
        return title;
    }
}

EmulationSession & EmulationSession::GetInstance()
{
    static EmulationSession instance;
    return instance;
}

void EmulationSession::InitializeSystem()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    RestoreStubModulesLocked();
    if (m_system_modules && m_system_modules->IsValid())
    {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "SystemModules: setup OK");
    }
    else
    {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "SystemModules: setup incomplete — ROM metadata will be unavailable");
        m_system_modules.reset();
    }
}

void EmulationSession::ShutdownSystem()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_system_modules.reset();
    m_render_window.ClearSurface();
    m_native_window = nullptr;
    m_pixel_ratio = 1.0f;
}

void EmulationSession::RestoreStubModulesLocked()
{
    m_system_modules.reset();
    {
        std::lock_guard events_lock(m_program_events_mutex);
        m_program_events.clear();
        m_module_generation++;
    }
    m_render_window.ClearSurface();
    m_native_window = nullptr;
    m_pixel_ratio = 1.0f;
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);
}

bool EmulationSession::Run(ANativeWindow * native_window, float pixel_ratio, const std::string & rom_path, uint64_t program_id)
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_system_modules.reset();

    if (native_window == nullptr)
    {
        RestoreStubModulesLocked();
        return false;
    }

    m_native_window = native_window;
    m_pixel_ratio = pixel_ratio > 0.f ? pixel_ratio : 1.0f;
    m_render_window.AttachSurface(native_window, m_pixel_ratio);
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);

    if (!m_system_modules->IsValid())
    {
        RestoreStubModulesLocked();
        return false;
    }

    const int native_w = ANativeWindow_getWidth(m_native_window);
    const int native_h = ANativeWindow_getHeight(m_native_window);
    if (native_w > 0 && native_h > 0)
    {
        m_system_modules->Modules().Video().UpdateFramebufferLayout(static_cast<uint32_t>(native_w),
                                                                 static_cast<uint32_t>(native_h));
    }

    m_rom_path = rom_path;
    m_program_id = program_id;
    if (!StartProgramLocked(program_id, 0, -1, ApplicationLaunchType::FrontendInitiated, {}))
    {
        RestoreStubModulesLocked();
        return false;
    }
    return true;
}

bool EmulationSession::StartProgramLocked(uint64_t program_id, int32_t program_index, int32_t previous_program_index,
                                          ApplicationLaunchType launch_type, const UserChannel & user_channel)
{
    if (!m_system_modules)
    {
        SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
        m_system_modules = std::make_unique<SystemModules>();
        m_system_modules->Setup(m_render_window);
        if (!m_system_modules->IsValid())
        {
            return false;
        }
        const int native_w = m_native_window != nullptr ? ANativeWindow_getWidth(m_native_window) : 0;
        const int native_h = m_native_window != nullptr ? ANativeWindow_getHeight(m_native_window) : 0;
        if (native_w > 0 && native_h > 0)
        {
            m_system_modules->Modules().Video().UpdateFramebufferLayout(static_cast<uint32_t>(native_w), static_cast<uint32_t>(native_h));
        }
    }

    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    m_callback_context = std::make_unique<CallbackContext>(CallbackContext{this, &os, ++m_module_generation});
    os.RegisterExecuteProgramCallback(ExecuteProgramCallback, m_callback_context.get());
    os.RegisterExitCallback(ExitProgramCallback, m_callback_context.get());
    for (const std::vector<uint8_t> & entry : user_channel)
    {
        os.PushUserChannelEntry(entry.data(), static_cast<uint32_t>(entry.size()));
    }

    if (!m_system_modules->Modules().Systemloader().LoadRom(m_rom_path.c_str(), program_id, program_index, previous_program_index, launch_type))
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "LoadRom failed for program %d of %s", program_index, m_rom_path.c_str());
        return false;
    }
    m_current_program_index = program_index;
    m_previous_program_index = previous_program_index;
    return true;
}

EmulationSession::ProgramEventResult EmulationSession::ProcessProgramEvents()
{
    std::deque<ProgramEvent> events;
    {
        std::lock_guard lock(m_program_events_mutex);
        events.swap(m_program_events);
    }
    std::lock_guard lock(m_mutex);
    ProgramEventResult result = ProgramEventResult::None;
    for (const ProgramEvent & event : events)
    {
        if (event.generation != m_module_generation || !m_system_modules || !m_system_modules->IsValid())
        {
            continue;
        }
        int32_t index = 0;
        const int32_t previous = m_current_program_index;
        if (event.type == ProgramEvent::Type::Execute)
        {
            index = static_cast<int32_t>(event.index);
        }
        else if (m_current_program_index != 0)
        {
            index = m_previous_program_index >= 0 ? m_previous_program_index : 0;
        }
        else
        {
            return ProgramEventResult::Exited;
        }

        m_system_modules.reset();
        if (!StartProgramLocked(m_program_id, index, previous, ApplicationLaunchType::ApplicationInitiated,
                                event.type == ProgramEvent::Type::Execute ? event.userChannel : UserChannel{}))
        {
            return ProgramEventResult::Failed;
        }
        result = ProgramEventResult::Reloaded;
    }
    return result;
}

void EmulationSession::QueueProgramEvent(ProgramEvent event)
{
    {
        std::lock_guard lock(m_program_events_mutex);
        m_program_events.push_back(std::move(event));
    }
    if (JNIEnv * env = GetEnvForThread(); env != nullptr)
    {
        jclass clazz = static_cast<jclass>(GetNativeLibraryGlobalRef());
        if (jmethodID method = env->GetStaticMethodID(clazz, "onProgramEvent", "()V"); method != nullptr)
        {
            env->CallStaticVoidMethod(clazz, method);
        }
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
}

void EmulationSession::CollectUserChannel(const uint8_t * data, uint32_t size, void * userData)
{
    UserChannel & channel = *static_cast<UserChannel *>(userData);
    if (data != nullptr && size != 0)
    {
        channel.emplace_back(data, data + size);
    }
    else
    {
        channel.emplace_back();
    }
}

void EmulationSession::ExecuteProgramCallback(uint32_t index, void * userData)
{
    const CallbackContext & context = *static_cast<CallbackContext *>(userData);
    ProgramEvent event{ProgramEvent::Type::Execute, context.generation, index, {}};
    context.os->ExportUserChannel(CollectUserChannel, &event.userChannel);
    context.session->QueueProgramEvent(std::move(event));
}

void EmulationSession::ExitProgramCallback(void * userData)
{
    const CallbackContext & context = *static_cast<CallbackContext *>(userData);
    context.session->QueueProgramEvent({ProgramEvent::Type::Exit, context.generation, 0, {}});
}

void EmulationSession::Stop()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    RestoreStubModulesLocked();
}

void EmulationSession::SetPaused(bool paused)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetEmulationPaused(paused);
}

bool EmulationSession::IsPaused()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return false;
    }
    return m_system_modules->Modules().OperatingSystem().IsEmulationPaused();
}

void EmulationSession::SetNativeWindow(ANativeWindow * native_window)
{
    std::lock_guard lock(m_mutex);
    m_render_window.AttachSurface(native_window, m_pixel_ratio);
    m_native_window = native_window;
}

void EmulationSession::SurfaceDestroyed()
{
    std::lock_guard lock(m_mutex);
    const bool keep_running = m_system_modules && m_system_modules->IsValid() &&
                              SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
    m_render_window.SuppressPresentation();
    if (keep_running)
    {
        m_system_modules->Modules().Video().NotifyWindowChanged();
    }
}

void EmulationSession::SurfaceChanged()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid() || m_native_window == nullptr)
    {
        return;
    }
    if (!SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning))
    {
        return;
    }
    IVideo & video = m_system_modules->Modules().Video();
    video.NotifyWindowChanged();
    const int native_w = ANativeWindow_getWidth(m_native_window);
    const int native_h = ANativeWindow_getHeight(m_native_window);
    if (native_w > 0 && native_h > 0)
    {
        video.UpdateFramebufferLayout(static_cast<uint32_t>(native_w), static_cast<uint32_t>(native_h));
    }
}

bool EmulationSession::ReadAppletCapture(std::vector<uint8_t> & out, uint32_t & width, uint32_t & height)
{
    IVideo * video = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_system_modules || !m_system_modules->IsValid())
        {
            return false;
        }
        video = &m_system_modules->Modules().Video();
    }
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    const uint32_t size = video->GetLinearAppletCaptureBuffer(nullptr, 0, &capture_width, &capture_height);
    if (size == 0 || capture_width == 0 || capture_height == 0)
    {
        return false;
    }
    out.resize(size);
    video->GetLinearAppletCaptureBuffer(out.data(), size, &capture_width, &capture_height);
    width = capture_width;
    height = capture_height;
    return width * height * 4 <= out.size();
}

void EmulationSession::AppletCaptureSize(uint32_t & width, uint32_t & height)
{
    width = 0;
    height = 0;
    IVideo * video = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_system_modules || !m_system_modules->IsValid())
        {
            return;
        }
        video = &m_system_modules->Modules().Video();
    }
    video->GetLinearAppletCaptureBuffer(nullptr, 0, &width, &height);
}

ANativeWindow * EmulationSession::NativeWindow() const
{
    std::lock_guard lock(m_mutex);
    return m_native_window;
}

float EmulationSession::PixelRatio() const
{
    std::lock_guard lock(m_mutex);
    return m_pixel_ratio;
}

std::string EmulationSession::QueryRomMetadata(const std::string & path)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {};
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    IRomInfo * const rom = loader.RomInfo(path.c_str(), 0, 0);
    if (rom == nullptr)
    {
        return {};
    }

    const LoaderFileType file_type = rom->GetFileType();

    uint64_t program_id = 0;
    const LoaderResultStatus pid_res = rom->ReadProgramId(program_id);
    const bool have_pid = pid_res == LoaderResultStatus::Success;

    const std::string title = ReadRomTitle(rom);
    const std::string icon_b64 = EncodeRomImage(rom, &IRomInfo::ReadIcon);

    rom->Release();

    char pid_hex[17];
    if (have_pid)
    {
        snprintf(pid_hex, sizeof(pid_hex), "%016llx", (unsigned long long)program_id);
    }
    else
    {
        snprintf(pid_hex, sizeof(pid_hex), "0");
    }

    JsonValue obj(JsonValueType::Object);
    obj["path"] = path;
    obj["title"] = title;
    obj["programId"] = pid_hex;
    obj["fileType"] = static_cast<int>(file_type);
    obj["icon"] = icon_b64;
    obj["error"] = JsonValue();
    return JsonStyledWriter().write(obj);
}

bool EmulationSession::QueryRomListing(const std::string & path, std::vector<RomListing> & games)
{
    std::lock_guard lock(m_mutex);
    games.clear();
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return false;
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    IRomInfo * const rom = loader.RomInfo(path.c_str(), 0, 0);
    if (rom == nullptr)
    {
        return false;
    }
    const auto listing = [](IRomInfo * info, uint64_t program_id) {
        return RomListing{program_id, ReadRomTitle(info), ReadRomImage(info, &IRomInfo::ReadIcon)};
    };

    const LoaderFileType file_type = rom->GetFileType();
    uint32_t count = 0;
    std::vector<uint64_t> program_ids;
    if ((file_type == LoaderFileType::XCI || file_type == LoaderFileType::NSP) &&
        rom->ReadProgramIds(nullptr, &count) == LoaderResultStatus::Success && count > 1)
    {
        program_ids.resize(count);
        if (rom->ReadProgramIds(program_ids.data(), &count) != LoaderResultStatus::Success)
        {
            program_ids.clear();
        }
        program_ids.resize(std::min<size_t>(count, program_ids.size()));
    }
    for (uint64_t program_id : program_ids)
    {
        IRomInfo * const program = loader.RomInfo(path.c_str(), program_id, 0);
        if (program == nullptr)
        {
            continue;
        }
        uint64_t read_id = 0;
        if (program->GetFileType() == file_type && program->ReadProgramId(read_id) == LoaderResultStatus::Success && read_id == program_id)
        {
            games.push_back(listing(program, program_id));
        }
        program->Release();
    }
    if (games.empty())
    {
        games.push_back(listing(rom, 0));
    }
    rom->Release();
    return true;
}

std::string EmulationSession::QueryRomInfo(const std::string & path, uint64_t program_id)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {};
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    IRomInfo * rom = nullptr;
    if (!path.empty())
    {
        rom = loader.RomInfo(path.c_str(), program_id, 0);
    }
    if (rom == nullptr)
    {
        rom = loader.LoadedRomInfo();
    }
    if (rom == nullptr)
    {
        return {};
    }

    JsonValue obj(JsonValueType::Object);
    obj["title"] = ReadRomTitle(rom);
    obj["icon"] = EncodeRomImage(rom, &IRomInfo::ReadIcon);
    obj["logo"] = EncodeRomImage(rom, &IRomInfo::ReadLogo);
    obj["banner"] = EncodeRomImage(rom, &IRomInfo::ReadBanner);
    rom->Release();
    return JsonStyledWriter().write(obj);
}

std::array<double, 4> EmulationSession::GetPerfStats()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {0.0, 0.0, 0.0, 0.0};
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return {0.0, 0.0, 0.0, 0.0};
    }
    const PerfStatsResults results = os.GetAndResetPerfStats();
    return {results.system_fps, results.average_game_fps, results.frametime, results.emulation_speed};
}

uint32_t EmulationSession::GetShadersBuilding()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return 0;
    }
    return m_system_modules->Modules().Video().ShadersBuilding();
}

std::string EmulationSession::GetFirmwareVersion()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return "N/A";
    }
    char buffer[32]{};
    const uint32_t length =
        m_system_modules->Modules().Systemloader().GetInstalledFirmwareDisplayVersion(buffer, sizeof(buffer));
    if (length == 0)
    {
        return "N/A";
    }
    return std::string(buffer, length);
}

std::string EmulationSession::GetGameCardFirmwareUpdate(const std::string & path)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {};
    }
    char version[64]{};
    const uint32_t length = m_system_modules->Modules().Systemloader().GetGameCardFirmwareUpdate(path.c_str(), version, sizeof(version));
    return std::string(version, length);
}

FirmwareInstallStatus EmulationSession::InstallFirmware(const std::string & path, IFirmwareInstallObserver & observer)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return FirmwareInstallStatus::StorageUnavailable;
    }
    char version[64]{};
    return m_system_modules->Modules().Systemloader().InstallFirmwarePackage(path.c_str(), &observer, version, sizeof(version));
}

void EmulationSession::SetOverlayButton(int port, int button_id, bool pressed)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetPlayerButtonState((uint32_t)port, (uint32_t)button_id, pressed);
}

void EmulationSession::SetOverlayJoystick(int port, int stick_id, float x, float y)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetPlayerAnalogState((uint32_t)port, (uint32_t)stick_id, x, y);
}

int EmulationSession::GetStyleIndex(int player_index)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return static_cast<int>(NpadStyleIndex::Fullkey);
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    const NpadIdType npad_id = player_index == 8 ? NpadIdType::Handheld : (NpadIdType)player_index;
    return (int)os.GetEmulatedController(npad_id).GetNpadStyleIndex();
}

IOperatingSystem * EmulationSession::OperatingSystem()
{
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return nullptr;
    }
    return &m_system_modules->Modules().OperatingSystem();
}

SystemModules * EmulationSession::Modules()
{
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return nullptr;
    }
    return m_system_modules.get();
}
