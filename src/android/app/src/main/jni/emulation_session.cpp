#include "emulation_session.h"

#include <android/log.h>
#include <cstdio>
#include <vector>

#include <common/base64.h>
#include <common/json.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>

namespace
{
    constexpr const char * kLogTag = "NxEmu";
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
    m_system_modules.reset();
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);
    if (m_system_modules->IsValid())
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
    m_system_modules.reset();
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

    std::string icon_b64;
    uint32_t icon_sz = 0;
    LoaderResultStatus icon_res = rom->ReadIcon(nullptr, &icon_sz);
    if (icon_res == LoaderResultStatus::Success && icon_sz > 0 && icon_sz < 16 * 1024 * 1024)
    {
        std::vector<uint8_t> icon(icon_sz);
        icon_res = rom->ReadIcon(icon.data(), &icon_sz);
        if (icon_res == LoaderResultStatus::Success && icon_sz > 0)
        {
            icon.resize(icon_sz);
            icon_b64 = base64_encode(icon.data(), icon.size());
        }
    }

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