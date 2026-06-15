#include "config_setting.h"
#include "settings/ui_settings.h"
#include "system_config.h"
#include "system_config_audio.h"
#include "system_config_general.h"
#include "system_config_graphics.h"
#include "system_config_system.h"
#include <common/std_string.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/notification.h>
#include <sciter_ui.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <widgets/page_nav.h>
#include <widgets/list_box.h>
#include <nxemu-module-spec/video.h>
#include <yuzu_common/settings_enums.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/core_settings.h>

SystemConfig::SystemConfig(ISciterUI & SciterUI, SystemModules & modules, std::vector<VkDeviceRecord> & vkDeviceRecords) :
    m_sciterUI(SciterUI),
    m_modules(modules),
    m_vkDeviceRecords(vkDeviceRecords),
    m_window(nullptr)
{
}

SystemConfig::~SystemConfig()
{
}

void SystemConfig::Display(void * parentWindow, const char * startPage)
{
    InitializeTranslations();

    enum
    {
        WINDOW_HEIGHT = 576,
        WINDOW_WIDTH = 840,
    };

    std::string initialPage(startPage != nullptr ? startPage : ""), subPage;
    const size_t separator = initialPage.find(':');
    if (separator != std::string::npos)
    {
        subPage = initialPage.substr(separator + 1);
        initialPage = initialPage.substr(0, separator);
    }
    if (!m_sciterUI.WindowCreate(parentWindow, "system_config.html", 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SUIW_CHILD, m_window))
    {
        return;
    }
    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement pageNav = root.GetElementByID("MainTabNav");
        std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
        if (interfacePtr)
        {
            m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
            m_pageNav->AddSink(this);
            if (!initialPage.empty())
            {
                m_pageNav->SetCurrentPage(initialPage.c_str());
                if (!subPage.empty())
                {
                    if (initialPage == "General" && m_systemConfigGeneral)
                    {
                        m_systemConfigGeneral->SetInitialPage(subPage.c_str());
                    }
                }
            }
        }
        SciterElement okButton = root.FindFirst("button[role=\"window-ok\"]");
        m_sciterUI.AttachHandler(okButton, IID_ICLICKSINK, (IClickSink*)this);
    }
    m_window->FixMinSize();
    m_window->CenterWindow();
}

void SystemConfig::SavePage(SCITER_ELEMENT pageElement, const ConfigSetting* settings, size_t settingsCount)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    SciterElement page(pageElement);

    for (size_t i = 0; i < settingsCount; ++i)
    {
        const ConfigSetting& setting = settings[i];
        if (setting.Type() == ConfigSettingType::ComboBox)
        {
            SaveComboBox(page, setting, true);
        }
        else if (setting.Type() == ConfigSettingType::ComboBoxValue)
        {
            SaveComboBox(page, setting, false);
        }
        else if (setting.Type() == ConfigSettingType::CheckBox)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                settingsStore.SetBool(setting.StoreSettingId(), (element.GetState() & SciterElement::STATE_CHECKED) != 0);
            }
        }
        else if (setting.Type() == ConfigSettingType::Slider)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                SciterValue value = element.GetValue();
                if (value.isInt())
                {
                    settingsStore.SetInt(setting.StoreSettingId(), value.GetValueInt());
                }
            }
        }
        else if (setting.Type() == ConfigSettingType::InputText)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                SciterValue value = element.GetValue();
                if (value.isString())
                {
                    settingsStore.SetString(setting.StoreSettingId(), value.GetValueStr().c_str());
                }
            }
        }
        else if (setting.Type() == ConfigSettingType::ListBox)
        {
            std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(page.GetElementByID(setting.ElementId()), IID_ILISTBOX);
            if (interfacePtr)
            {
                JsonValue jsonArray;
                std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
                for (uint32_t item = 0, n = listBox->GetCount(); item < n; item++)
                {
                    SciterElement element = listBox->GetItem(item);
                    if (element)
                    {
                        std::string value = element.GetAttribute("value");
                        if (!value.empty())
                        {
                            jsonArray.Append(value);                        
                        }
                    }
                }

                if (jsonArray.isArray())
                {
                    std::string jsonOutput = JsonStyledWriter().write(jsonArray);
                    settingsStore.SetString(setting.StoreSettingId(), jsonOutput.c_str());
                }
                else
                {
                    settingsStore.SetString(setting.StoreSettingId(), "");
                
                }
            }
        }
        else
        {
            g_notify->BreakPoint(__FILE__, __LINE__);
        }
    }
}

void SystemConfig::SetupPage(SCITER_ELEMENT pageElement, const ConfigSetting * settings, size_t settingsCount)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    SciterElement page(pageElement);
    bool emulationRunning = settingsStore.GetBool(NXCoreSetting::EmulationRunning);

    for (size_t i = 0; i < settingsCount; ++i) 
    {
        const ConfigSetting & setting = settings[i];
        if (emulationRunning && !setting.CanChangeWhenRunning())
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                element.SetState(SciterElement::STATE_DISABLED, 0, true);
                element = page.FindFirst("[for='%s']", setting.ElementId());
                if (element)
                {
                    element.SetState(SciterElement::STATE_DISABLED, 0, true);
                }
            }
        }
        if (setting.Type() == ConfigSettingType::ListBox)
        {
            std::string settingValue = settingsStore.GetString(setting.StoreSettingId());

            JsonReader reader;
            JsonValue value;
            if (!settingValue.empty() && reader.Parse(settingValue.c_str(), settingValue.c_str() + settingValue.length(), value) && value.isArray())
            {
                std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(page.GetElementByID(setting.ElementId()), IID_ILISTBOX);
                if (interfacePtr)
                {
                    std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
                    for (uint32_t strIndex = 0; strIndex < value.size(); strIndex++)
                    {
                        if (!value[strIndex].isString())
                        {
                            continue;
                        }
                        listBox->AddItem(value[strIndex].asString().c_str(), value[strIndex].asString().c_str());
                    }
                }
            }
        }
        else if (setting.Type() == ConfigSettingType::ComboBox)
        {
            SetupComboBox(page, setting);
        }
        else if (setting.Type() == ConfigSettingType::ComboBoxValue)
        {
            // do nothing
        }
        else if (setting.Type() == ConfigSettingType::CheckBox)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                bool checked = settingsStore.GetBool(setting.StoreSettingId());
                element.SetState(checked ? SciterElement::STATE_CHECKED : 0, checked ? 0 : SciterElement::STATE_CHECKED, true);
            }
        }
        else if (setting.Type() == ConfigSettingType::Slider)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                element.SetValue(SciterValue(settingsStore.GetInt(setting.StoreSettingId())));
            }
        }
        else if (setting.Type() == ConfigSettingType::InputText)
        {
            SciterElement element = page.GetElementByID(setting.ElementId());
            if (element)
            {
                element.SetValue(SciterValue(std::string(settingsStore.GetString(setting.StoreSettingId()))));
            }
        }
        else
        {
            g_notify->BreakPoint(__FILE__, __LINE__);
        }
    }
}

void SystemConfig::SaveComboBox(const SciterElement & page, const ConfigSetting & setting, bool intValue)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(page.GetElementByID(setting.ElementId()), IID_ICOMBOBOX);
    if (interfacePtr)
    {
        std::shared_ptr<IComboBox> comboBox = std::static_pointer_cast<IComboBox>(interfacePtr);
        SciterElement element = comboBox->GetSelectedItem();
        if (element)
        {
            std::string value = element.GetAttribute("value");
            if (value.size() > 0)
            {
                if (intValue)
                {
                    settingsStore.SetInt(setting.StoreSettingId(), std::stoi(value.c_str()));
                }
                else
                {
                    settingsStore.SetString(setting.StoreSettingId(), value.c_str());
                }
            }
        }
    }
}

void SystemConfig::SetupComboBox(const SciterElement & page, const ConfigSetting & setting)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(page.GetElementByID(setting.ElementId()), IID_ICOMBOBOX);
    SettingTranslationMap::iterator itr = m_settingTranslations.find(setting.SettingIndex());
    if (interfacePtr && itr != m_settingTranslations.end())
    {
        int32_t settingValue = settingsStore.GetInt(setting.StoreSettingId());
        std::shared_ptr<IComboBox> comboBox = std::static_pointer_cast<IComboBox>(interfacePtr);
        SettingTranslationList & translation = itr->second;
        int32_t selectedIndex = -1;
        for (size_t i = 0, n = translation.size(); i < n; i++)
        {
            int32_t index = comboBox->AddItem(translation[i].second.c_str(), stdstr_f("%d", translation[i].first).c_str());
            if (settingValue == translation[i].first)
            {
                selectedIndex = index;
            }
        }
        if (selectedIndex >= 0)
        {
            comboBox->SelectItem(selectedIndex);
        }
    }
}

bool SystemConfig::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfig::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfig::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "Audio")
    {
        m_systemConfigAudio.reset(new SystemConfigAudio(m_sciterUI, *this, m_modules, m_window->GetHandle(), page));
    }
    else if (pageName == "Graphics")
    {
        m_systemConfigGraphics.reset(new SystemConfigGraphics(m_sciterUI, *this, m_window->GetHandle(), page));
    }
    else if (pageName == "General")
    {
        m_systemConfigGeneral.reset(new SystemConfigGeneral(m_sciterUI, *this, *m_window, page));
    }
}

void SystemConfig::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}

bool SystemConfig::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    if (clickElem.GetAttribute("role") == "window-ok")
    {
        if (m_systemConfigAudio)
        {
            m_systemConfigAudio->SaveSetting();
        }
        if (m_systemConfigGraphics)
        {
            m_systemConfigGraphics->SaveSetting();
        }
        if (m_systemConfigGeneral)
        {
            m_systemConfigGeneral->SaveSetting();
        }
        if (m_modules.IsValid())
        {
            m_modules.FlushSettings();
        }
        SaveCoreSetting();
        SaveUISetting();
        m_window->Destroy();
    }
    return true;
}

void SystemConfig::InitializeTranslations()
{
    if (!m_settingTranslations.empty())
    {
        return;
    }

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::RendererBackend, {
        {(uint32_t)RendererBackend::OpenGL, "OpenGL"},
        {(uint32_t)RendererBackend::Vulkan, "Vulkan"},
        {(uint32_t)RendererBackend::Null, "Null"},
    }});

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::ShaderBackend, {
        {(uint32_t)ShaderBackend::Glsl, "GLSL"},
        {(uint32_t)ShaderBackend::Glasm, "GLASM (Assembly Shaders, NVIDIA Only)"},
        {(uint32_t)ShaderBackend::SpirV, "SPIR-V (Experimental, AMD/Mesa Only)"},
    }});

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::AstcDecodeMode, {
        {(uint32_t)AstcDecodeMode::Cpu, "CPU"},
        {(uint32_t)AstcDecodeMode::Gpu, "GPU"},
        {(uint32_t)AstcDecodeMode::CpuAsynchronous, "CPU Asynchronous"},
    }});

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::NvdecEmulation, {
        {(uint32_t)NvdecEmulation::Off, "No Video Output"},
        {(uint32_t)NvdecEmulation::Cpu, "CPU Video Decoding"},
        {(uint32_t)NvdecEmulation::Gpu, "GPU Video Decoding (Default)"},
    }});

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::FullscreenMode, {
        {(uint32_t)FullscreenMode::Borderless, "Borderless Windowed"},
        {(uint32_t)FullscreenMode::Exclusive, "Exclusive Fullscreen"},
    }});

    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::AspectRatio, {
        {(uint32_t)AspectRatio::R16_9, "Default (16:9)"},
        {(uint32_t)AspectRatio::R4_3, "Force 4:3"},
        {(uint32_t)AspectRatio::R21_9, "Force 21:9"},
        {(uint32_t)AspectRatio::R16_10, "Force 16:10"},
        {(uint32_t)AspectRatio::Stretch, "Stretch to Window"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::ResolutionSetup, {
        {(uint32_t)ResolutionSetup::Res1_2X, "0.5X (360p/540p) [EXPERIMENTAL]"},
        {(uint32_t)ResolutionSetup::Res3_4X, "0.75X (540p/810p) [EXPERIMENTAL]"},
        {(uint32_t)ResolutionSetup::Res1X, "1X (720p/1080p)"},
        {(uint32_t)ResolutionSetup::Res3_2X, "1.5X (1080p/1620p) [EXPERIMENTAL]"},
        {(uint32_t)ResolutionSetup::Res2X, "2X (1440p/2160p)"},
        {(uint32_t)ResolutionSetup::Res3X, "3X (2160p/3240p)"},
        {(uint32_t)ResolutionSetup::Res4X, "4X (2880p/4320p)"},
        {(uint32_t)ResolutionSetup::Res5X, "5X (3600p/5400p)"},
        {(uint32_t)ResolutionSetup::Res6X, "6X (4320p/6480p)"},
        {(uint32_t)ResolutionSetup::Res7X, "7X (5040p/7560p)"},
        {(uint32_t)ResolutionSetup::Res8X, "8X (5760p/8640p)"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::ScalingFilter, {
        {(uint32_t)ScalingFilter::NearestNeighbor, "Nearest Neighbor"},
        {(uint32_t)ScalingFilter::Bilinear, "Bilinear"},
        {(uint32_t)ScalingFilter::Bicubic, "Bicubic"},
        {(uint32_t)ScalingFilter::Gaussian, "Gaussian"},
        {(uint32_t)ScalingFilter::ScaleForce, "ScaleForce"},
        {(uint32_t)ScalingFilter::Fsr, "AMD FidelityFX Super Resolution"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::AntiAliasing, {
        {(uint32_t)AntiAliasing::None, "None"},
        {(uint32_t)AntiAliasing::Fxaa, "FXAA"},
        {(uint32_t)AntiAliasing::Smaa, "SMAA"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::GpuAccuracy, {
        {(uint32_t)GpuAccuracy::Normal, "Normal"},
        {(uint32_t)GpuAccuracy::High, "High"},
        {(uint32_t)GpuAccuracy::Extreme, "Extreme"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::AnisotropyMode, {
        {(uint32_t)AnisotropyMode::Automatic, "Automatic"},
        {(uint32_t)AnisotropyMode::Default, "Default"},
        {(uint32_t)AnisotropyMode::X2, "2x"},
        {(uint32_t)AnisotropyMode::X4, "4x"},
        {(uint32_t)AnisotropyMode::X8, "8x"},
        {(uint32_t)AnisotropyMode::X16, "16x"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::AstcRecompression, {
        {(uint32_t)AstcRecompression::Uncompressed, "Uncompressed (Best quality)"},
        {(uint32_t)AstcRecompression::Bc1, "BC1 (Low quality)"},
        {(uint32_t)AstcRecompression::Bc3, "BC3 (Medium quality)"},
    }});
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::VramUsageMode, {
        {(uint32_t)VramUsageMode::Conservative, "Conservative"},
        {(uint32_t)VramUsageMode::Aggressive, "Aggressive"},
    }});
    SettingTranslationList vulkanDeviceTranslations;
    for (size_t i = 0; i < m_vkDeviceRecords.size(); ++i)
    {
        vulkanDeviceTranslations.push_back({ (int32_t)i, m_vkDeviceRecords[i].name });
    }
    m_settingTranslations.insert({ (uint32_t)VideoSettingTranslationType::VulkanDevice, vulkanDeviceTranslations });

    m_settingTranslations.insert({ Settings::EnumMetadata<Settings::AudioMode>::Index(), {
        {(uint32_t)Settings::AudioMode::Mono, "Mono"},
        {(uint32_t)Settings::AudioMode::Stereo, "Stereo"},
        {(uint32_t)Settings::AudioMode::Surround, "Surround"},
    }});

    m_settingTranslations.insert({Settings::EnumMetadata<Settings::DockedMode>::Index(), {
        {(uint32_t)Settings::DockedMode::Handheld, "Handheld"},
        {(uint32_t)Settings::DockedMode::Docked, "Docked"},
    }});
}