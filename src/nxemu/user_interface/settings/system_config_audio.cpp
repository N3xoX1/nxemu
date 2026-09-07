#include "config_setting.h"
#include "system_config_audio.h"
#include "system_config.h"
#include <common/std_string.h>
#include <cstring>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-module-spec/operating_system.h>
#include <yuzu_audio_core/audio_types.h>
#include <widgets/combo_box.h>
namespace
{
static ConfigSetting audioSettings[] = {
    ConfigSetting(ConfigSetting::ComboBox, "audioOutputEngine", false, SystemConfig::TranslationType::AudioEngine, NXOsSetting::AudioSinkId),
    ConfigSetting(ConfigSetting::ComboBoxValue, "audioOutputDevice", false, NXOsSetting::AudioOutputDeviceId),
    ConfigSetting(ConfigSetting::ComboBoxValue, "audioInputDevice", false, NXOsSetting::AudioInputDeviceId),
    ConfigSetting(ConfigSetting::ComboBox, "soundMode", true, SystemConfig::TranslationType::AudioMode, NXOsSetting::AudioMode),
    ConfigSetting(ConfigSetting::Slider, "audioVolume", true, NXOsSetting::AudioVolume),
    ConfigSetting(ConfigSetting::CheckBox, "muteAudio", true, NXOsSetting::AudioMuted),
};

void AddDeviceToVector(const char * device, void * userData)
{
    std::vector<std::string> * deviceList = (std::vector<std::string> *)userData;
    deviceList->emplace_back(device);
}
}

SystemConfigAudio::SystemConfigAudio(ISciterUI & sciterUI, SystemConfig & config, SystemModules & modules, HWINDOW parent, SciterElement page) :
    m_sciterUI(sciterUI),
    m_modules(modules),
    m_config(config),
    m_parent(parent),
    m_page(page)
{
    SciterElement pageNav = page.GetElementByID("AudioTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
}

void SystemConfigAudio::SaveSetting(void)
{
    if (m_audioPage != nullptr)
    {
        m_config.SavePage(m_audioPage, audioSettings, sizeof(audioSettings) / sizeof(audioSettings[0]));
    }
}

bool SystemConfigAudio::PageNavChangeFrom(const std::string& /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigAudio::PageNavChangeTo(const std::string& /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigAudio::PageNavCreatedPage(const std::string& pageName, SCITER_ELEMENT page)
{
    if (pageName == "Audio")
    {
        SetupAudioPage(page);
    }
}

void SystemConfigAudio::PageNavPageChanged(const std::string& /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}

void SystemConfigAudio::SetupAudioPage(SciterElement page)
{
    m_audioPage = page;
    m_config.SetupPage(page, audioSettings, sizeof(audioSettings) / sizeof(audioSettings[0]));
    if (!m_modules.IsValid())
    {
        return;
    }
    IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();

    uint32_t count = 0;
    operatingSystem.AudioGetSyncIDs(nullptr, 0, &count);
    std::vector<uint32_t> ids(count);
    operatingSystem.AudioGetSyncIDs(ids.data(), count, &count);

    SettingsStore& settings = SettingsStore::GetInstance();
    int32_t audioSinkId = settings.GetInt(NXOsSetting::AudioSinkId);
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(page.GetElementByID("audioOutputEngine"), IID_ICOMBOBOX);
    if (interfacePtr)
    {
        m_outputEngine = std::static_pointer_cast<IComboBox>(interfacePtr);
        int32_t index = m_outputEngine->AddItem(m_config.GetSettingLabel(SystemConfig::TranslationType::AudioEngine, (int32_t)AudioCore::Sink::AudioEngine::Auto), stdstr_f("%d", (int32_t)AudioCore::Sink::AudioEngine::Auto).c_str());
        int32_t selectedIndex = index;
        for (size_t i = 0, n = ids.size(); i < n; i++)
        {
            index = m_outputEngine->AddItem(m_config.GetSettingLabel(SystemConfig::TranslationType::AudioEngine, (int32_t)ids[i]), stdstr_f("%d", ids[i]).c_str());
            if (audioSinkId == (int32_t)ids[i])
            {
                selectedIndex = index;
            }
        }
        if (selectedIndex >= 0)
        {
            m_outputEngine->SelectItem(selectedIndex);
        }
    }
    const char* audioOutputDeviceId = settings.GetString(NXOsSetting::AudioOutputDeviceId);
    const char* audioInputDeviceId = settings.GetString(NXOsSetting::AudioInputDeviceId);
    updateAudioDevices(audioSinkId, audioOutputDeviceId, audioInputDeviceId);
    m_sciterUI.AttachHandler(page.GetElementByID("audioOutputEngine"), IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    m_sciterUI.AttachHandler(page.GetElementByID("audioVolume"), IID_ISTATECHANGESINK, (IStateChangeSink*)this);
}

bool SystemConfigAudio::OnStateChange(SCITER_ELEMENT elem, uint32_t /*eventReason*/, void * /*data*/)
{
    if (m_page.GetElementByID("audioOutputEngine") == elem)
    {
        SciterElement element = m_outputEngine->GetSelectedItem();
        if (!element)
        {
            return false;
        }
        std::string value = element.GetAttribute("value");
        if (value.empty())
        {
            return false;
        }
        int audioSinkId = std::stoi(value);
        SettingsStore & settings = SettingsStore::GetInstance();
        const char * audioOutputDeviceId = settings.GetDefaultString(NXOsSetting::AudioOutputDeviceId);
        const char * audioInputDeviceId = settings.GetDefaultString(NXOsSetting::AudioInputDeviceId);
        updateAudioDevices(audioSinkId, audioOutputDeviceId, audioInputDeviceId);
    }
    else if (m_page.GetElementByID("audioVolume") == elem)
    {
        updateVolumeDisplay();
    }
    return false;
}

void SystemConfigAudio::updateVolumeDisplay()
{
    SciterElement audioVolume = m_page.GetElementByID("audioVolume");
    SciterElement volumeDisplay = m_page.GetElementByID("VolumeDisplay");

    if (audioVolume && volumeDisplay)
    {
        SciterValue value = audioVolume.GetValue();
        if (value.isInt())
        {
            stdstr_f text("%d %%", value.GetValueInt());
            volumeDisplay.SetHTML((const uint8_t*)text.c_str(), text.size());
        }
    }
}

void SystemConfigAudio::updateAudioDevices(int32_t audioSinkId, const char * audioOutputDeviceId, const char * audioInputDeviceId)
{
    if (!m_modules.IsValid())
    {
        return;
    }
    IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_page.GetElementByID("audioOutputDevice"), IID_ICOMBOBOX);
    if (interfacePtr)
    {
        m_audioOutputDevice = std::static_pointer_cast<IComboBox>(interfacePtr);
        m_audioOutputDevice->ClearContents();
        std::vector<std::string> devices;
        operatingSystem.AudioGetDeviceListForSink(audioSinkId, false, AddDeviceToVector, &devices);
        int32_t index = m_audioOutputDevice->AddItem("auto", "auto");
        int32_t selectedIndex = index;
        for (size_t i = 0, n = devices.size(); i < n; i++)
        {
            index = m_audioOutputDevice->AddItem(devices[i].c_str(), devices[i].c_str());
            if (strcmp(audioOutputDeviceId, devices[i].c_str()) == 0)
            {
                selectedIndex = index;
            }
        }
        if (selectedIndex >= 0)
        {
            m_audioOutputDevice->SelectItem(selectedIndex);
        }
    }

    interfacePtr = m_sciterUI.GetElementInterface(m_page.GetElementByID("audioInputDevice"), IID_ICOMBOBOX);
    if (interfacePtr)
    {
        m_audioInputDevice = std::static_pointer_cast<IComboBox>(interfacePtr);
        m_audioInputDevice->ClearContents();
        std::vector<std::string> devices;
        operatingSystem.AudioGetDeviceListForSink(audioSinkId, true, AddDeviceToVector, &devices);
        int32_t index = m_audioInputDevice->AddItem("auto", "auto");
        int32_t selectedIndex = index;
        for (size_t i = 0, n = devices.size(); i < n; i++)
        {
            index = m_audioInputDevice->AddItem(devices[i].c_str(), devices[i].c_str());
            if (strcmp(audioInputDeviceId, devices[i].c_str()) == 0)
            {
                selectedIndex = index;
            }
        }
        if (selectedIndex >= 0)
        {
            m_audioInputDevice->SelectItem(selectedIndex);
        }
    }
}
