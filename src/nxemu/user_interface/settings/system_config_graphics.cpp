#include "system_config_graphics.h"
#include "config_setting.h"
#include "system_config.h"
#include <common/std_string.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu-module-spec/video.h>
#include <widgets/combo_box.h>

namespace 
{
    static ConfigSetting graphicsSettings[] = {
        ConfigSetting(ConfigSetting::ComboBox, "GraphicsAPI", false, SystemConfig::TranslationType::RendererBackend, NXVideoSetting::GraphicsAPI),
        ConfigSetting(ConfigSetting::ComboBox, "ShaderBackend", false, SystemConfig::TranslationType::ShaderBackend, NXVideoSetting::ShaderBackend),
        ConfigSetting(ConfigSetting::ComboBox, "VulkanDevices", false, SystemConfig::TranslationType::VulkanDevice, NXVideoSetting::VulkanDevice),
        ConfigSetting(ConfigSetting::CheckBox, "UseDiskPipelineCache", false, NXVideoSetting::UseDiskPipelineCache),
        ConfigSetting(ConfigSetting::CheckBox, "UseAsynchronousGPUEmulation", false, NXVideoSetting::UseAsynchronousGPUEmulation),
        ConfigSetting(ConfigSetting::ComboBox, "AstcDecodeMode", false, SystemConfig::TranslationType::AstcDecodeMode, NXVideoSetting::AstcDecodeMode),
        ConfigSetting(ConfigSetting::ComboBox, "VSyncMode", true, SystemConfig::TranslationType::VSyncMode, NXVideoSetting::VSyncMode),
        ConfigSetting(ConfigSetting::ComboBox, "NvdecEmulation", false, SystemConfig::TranslationType::NvdecEmulation, NXVideoSetting::NvdecEmulation),
        ConfigSetting(ConfigSetting::ComboBox, "AspectRatio", true, SystemConfig::TranslationType::AspectRatio, NXVideoSetting::AspectRatio),
        ConfigSetting(ConfigSetting::ComboBox, "ResolutionSetup", false, SystemConfig::TranslationType::ResolutionSetup, NXVideoSetting::ResolutionSetup),
        ConfigSetting(ConfigSetting::ComboBox, "ScalingFilter", true, SystemConfig::TranslationType::ScalingFilter, NXVideoSetting::ScalingFilter),
        ConfigSetting(ConfigSetting::ComboBox, "AntiAliasing", true, SystemConfig::TranslationType::AntiAliasing, NXVideoSetting::AntiAliasing),
        ConfigSetting(ConfigSetting::Slider, "FSPSharpness", true, NXVideoSetting::FSPSharpness),
    };

    static ConfigSetting advancedSettings[] = {
        ConfigSetting(ConfigSetting::ComboBox, "AccuracyLevel", true, SystemConfig::TranslationType::GpuAccuracy, NXVideoSetting::AccuracyLevel),
        ConfigSetting(ConfigSetting::ComboBox, "DMAAccuracy", true, SystemConfig::TranslationType::DmaAccuracy, NXVideoSetting::DMAAccuracy),
        ConfigSetting(ConfigSetting::CheckBox, "SyncMemoryOperations", true, NXVideoSetting::SyncMemoryOperations),
        ConfigSetting(ConfigSetting::ComboBox, "AnisotropicFiltering", false, SystemConfig::TranslationType::AnisotropyMode, NXVideoSetting::AnisotropicFiltering),
        ConfigSetting(ConfigSetting::ComboBox, "ASTCRecompressionMethod", false, SystemConfig::TranslationType::AstcRecompression, NXVideoSetting::ASTCRecompressionMethod),
        ConfigSetting(ConfigSetting::ComboBox, "VRAMUsageMode", false, SystemConfig::TranslationType::VramUsageMode, NXVideoSetting::VRAMUsageMode),
        ConfigSetting(ConfigSetting::CheckBox, "EnableAsynchronousPresentation", false, NXVideoSetting::EnableAsynchronousPresentation),
        ConfigSetting(ConfigSetting::CheckBox, "ForceMaximumClocks", false, NXVideoSetting::ForceMaximumClocks),
        ConfigSetting(ConfigSetting::CheckBox, "EnableReactiveFlushing", false, NXVideoSetting::EnableReactiveFlushing),
        ConfigSetting(ConfigSetting::CheckBox, "UseAsynchronousShaderBuilding", false, NXVideoSetting::UseAsynchronousShaderBuilding),
        ConfigSetting(ConfigSetting::CheckBox, "FastGPUTime", true, NXVideoSetting::FastGPUTime),
        ConfigSetting(ConfigSetting::CheckBox, "UseVulkanPipelineCache", true, NXVideoSetting::UseVulkanPipelineCache),
        ConfigSetting(ConfigSetting::CheckBox, "SyncToFramerateOfVideoPlayback", false, NXVideoSetting::SyncToFramerateOfVideoPlayback),
        ConfigSetting(ConfigSetting::CheckBox, "BarrierFeedbackLoops", false, NXVideoSetting::BarrierFeedbackLoops),
    };

    RendererBackend RendererModeSelcted(ISciterUI & sciterUI, SciterElement & graphicsPage)
    {
        std::shared_ptr<void> interfacePtr = graphicsPage ? sciterUI.GetElementInterface(graphicsPage.GetElementByID("GraphicsAPI"), IID_ICOMBOBOX) : nullptr;
        if (!interfacePtr)
        {
            return RendererBackend::Null;
        }
        std::shared_ptr<IComboBox> comboBox = std::static_pointer_cast<IComboBox>(interfacePtr);
        SciterElement element = comboBox->GetSelectedItem();
        if (!element)
        {
            return RendererBackend::Null;
        }
        std::string value = element.GetAttribute("value");
        if (value.empty())
        {
            return RendererBackend::Null;
        }
        RendererBackend backend = (RendererBackend)std::stoi(value.c_str());
        return backend;
    }

}

SystemConfigGraphics::SystemConfigGraphics(ISciterUI & sciterUI, SystemConfig & config, HWINDOW parent, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_parent(parent),
    m_page(page),
    m_graphicsPage(nullptr),
    m_advancedPage(nullptr)
{
    SciterElement pageNav = page.GetElementByID("GraphicsTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
    m_sciterUI.AttachHandler(page.GetElementByID("FSPSharpness"), IID_ISTATECHANGESINK, (IStateChangeSink*)this);
}

void SystemConfigGraphics::SaveSetting(void)
{
    if (m_graphicsPage != nullptr)
    {
        m_config.SavePage(m_graphicsPage, graphicsSettings, sizeof(graphicsSettings) / sizeof(graphicsSettings[0]));
    }
    if (m_advancedPage != nullptr)
    {
        m_config.SavePage(m_advancedPage, advancedSettings, sizeof(advancedSettings) / sizeof(advancedSettings[0]));
    }
}

bool SystemConfigGraphics::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigGraphics::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigGraphics::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "Graphics")
    {
        SetupGraphicsPage(page);
    }
    else if (pageName == "Advanced")
    {
        SetupAdvancedPage(page);
    }
}

void SystemConfigGraphics::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}

bool SystemConfigGraphics::OnStateChange(SCITER_ELEMENT elem, uint32_t /*eventReason*/, void* /*data*/)
{
    if (m_graphicsPage && elem == m_graphicsPage.GetElementByID("GraphicsAPI"))
    {
        UpdateGraphicsAPI();
        UpdateVSyncMode();
    }
    else if (m_graphicsPage && elem == m_graphicsPage.GetElementByID("FSPSharpness"))
    {
        UpdateFSPSharpnessDisplay();
    }
    return false;
}

void SystemConfigGraphics::SetupAdvancedPage(SciterElement page)
{
    m_advancedPage = page;
    m_config.SetupPage(page, advancedSettings, sizeof(advancedSettings) / sizeof(advancedSettings[0]));
}

void SystemConfigGraphics::SetupGraphicsPage(SciterElement page)
{
    m_graphicsPage = page;
    m_config.SetupPage(page, graphicsSettings, sizeof(graphicsSettings) / sizeof(graphicsSettings[0]));
    m_sciterUI.AttachHandler(page.GetElementByID("GraphicsAPI"), IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    UpdateGraphicsAPI();
    UpdateVSyncMode();
    UpdateFSPSharpnessDisplay();
}

void SystemConfigGraphics::UpdateGraphicsAPI()
{
    SciterElement ShaderBackend = m_graphicsPage.GetElementByID("ShaderBackendRow");
    SciterElement VulkanDevices = m_graphicsPage.GetElementByID("VulkanDevicesRow");
    RendererBackend backend = RendererModeSelcted(m_sciterUI, m_graphicsPage);

    switch (backend)
    {
    case RendererBackend::OpenGL:
        ShaderBackend.SetStyleAttribute("display", "block");
        VulkanDevices.SetStyleAttribute("display", "none");
        break;
    case RendererBackend::Vulkan:
        ShaderBackend.SetStyleAttribute("display", "none");
        VulkanDevices.SetStyleAttribute("display", "block");
        break;
    case RendererBackend::Null:
        ShaderBackend.SetStyleAttribute("display", "none");
        VulkanDevices.SetStyleAttribute("display", "none");
        break;
    }
}

void SystemConfigGraphics::UpdateFSPSharpnessDisplay()
{
    if (!m_graphicsPage)
    {
        return;
    }
    SciterElement fspSharpness = m_graphicsPage.GetElementByID("FSPSharpness");
    SciterElement fspSharpnessDisplay = m_graphicsPage.GetElementByID("FSPSharpnessDisplay");

    if (fspSharpness && fspSharpnessDisplay)
    {
        SciterValue value = fspSharpness.GetValue();
        if (value.isInt())
        {
            stdstr_f text("%d %%", value.GetValueInt());
            fspSharpnessDisplay.SetHTML((const uint8_t*)text.c_str(), text.size());
        }
    }
}

void SystemConfigGraphics::UpdateVSyncMode()
{
    RendererBackend backend = RendererModeSelcted(m_sciterUI, m_graphicsPage);
    SciterElement vsyncMode = m_graphicsPage.GetElementByID("VSyncMode");
    std::shared_ptr<void> interfacePtr = m_graphicsPage ? m_sciterUI.GetElementInterface(vsyncMode, IID_ICOMBOBOX) : nullptr;
    if (!interfacePtr)
    {
        return;
    }
    std::shared_ptr<IComboBox> comboBox = std::static_pointer_cast<IComboBox>(interfacePtr);
    SciterElement currentItem = comboBox->GetSelectedItem();
    std::string currentValue;
    if (currentItem)
    {
        currentValue = currentItem.GetAttribute("value");
    }
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    int32_t settingValue = settingsStore.GetInt(NXVideoSetting::VSyncMode);
    int32_t defaultSettingValue = settingsStore.GetDefaultInt(NXVideoSetting::VSyncMode);
    SciterElement element = comboBox->GetSelectedItem();
    if (element)
    {
        std::string value = element.GetAttribute("value");
        if (value.size() > 0)
        {
            settingValue = std::stoi(value.c_str());
        }
    }

    std::vector<std::pair<int32_t, std::string>> vsyncOptions;

    switch (backend)
    {
    case RendererBackend::OpenGL:
        vsyncOptions.push_back({(int)VSyncMode::Immediate, "Off"});
        vsyncOptions.push_back({(int)VSyncMode::Fifo, "On"});
        break;
    case RendererBackend::Vulkan:
        vsyncOptions.push_back({ (int)VSyncMode::Immediate, "Immediate (VSync Off)" });
        vsyncOptions.push_back({ (int)VSyncMode::Mailbox, "Mailbox (Recommended)" });
        vsyncOptions.push_back({ (int)VSyncMode::Fifo, "FIFO (VSync On)" });
        vsyncOptions.push_back({ (int)VSyncMode::FifoRelaxed, "FIFO Relaxed" });
        break;
    case RendererBackend::Null:
        break;
    }
    comboBox->ClearContents();
    int32_t selectedIndex = -1, defaultSelectedIndex = -1;
    if (vsyncOptions.size() > 0)
    {
        vsyncMode.RemoveAttribute("disabled");
    }
    else
    {
        vsyncMode.SetAttribute("disabled", "");
    }

    for (size_t i = 0; i < vsyncOptions.size(); i++)
    {
        int32_t index = comboBox->AddItem(vsyncOptions[i].second.c_str(), stdstr_f("%d", vsyncOptions[i].first).c_str());
        if (settingValue == vsyncOptions[i].first)
        {
            selectedIndex = index;
        }
        if (defaultSettingValue == vsyncOptions[i].first)
        {
            defaultSelectedIndex = index;
        }
        
    }
    if (selectedIndex >= 0)
    {
        comboBox->SelectItem(selectedIndex);
    }
    else if (defaultSelectedIndex >= 0)
    {
        comboBox->SelectItem(defaultSelectedIndex);
    }
}