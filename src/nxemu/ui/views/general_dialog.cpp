#include "general_dialog.h"
#include "resolution_scales.h"
#include "scaling_filters.h"
#include "rml_events.h"
#include "settings_controls.h"
#include "user_profiles.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-os/os_types.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/settings/ui_settings.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/interface_language.h>

using namespace SettingsControls;

namespace
{
constexpr const char * kRegions[] = {TrMark("Japan"), TrMark("Americas"), TrMark("Europe"), TrMark("Australia"), TrMark("China"), TrMark("Korea"), TrMark("Taiwan")};

Choices Aspects()
{
    const std::string modified = Tr("For games modified to draw this shape. The others are squeezed into it.");
    return {
        Numbered("16:9", static_cast<int>(AspectRatio::R16_9), Tr("The shape of the console's picture. The right choice for nearly every game.")),
        Numbered("4:3", static_cast<int>(AspectRatio::R4_3), modified),
        Numbered("21:9", static_cast<int>(AspectRatio::R21_9), modified),
        Numbered("16:10", static_cast<int>(AspectRatio::R16_10), modified),
        Numbered(Tr("Stretch to the window"), static_cast<int>(AspectRatio::Stretch), Tr("Fills the window whatever its shape. The picture is deformed.")),
    };
}

Choices VSyncs()
{
    return {
        Numbered(Tr("On"), static_cast<int>(VSyncMode::Fifo), Tr("Each picture waits for the screen. No tearing: the usual choice.")),
        Numbered(Tr("On, late pictures tear"), static_cast<int>(VSyncMode::FifoRelaxed), Tr("As On, but a picture that comes late is shown at once. It tears instead of stuttering.")),
        Numbered(Tr("On, lowest delay"), static_cast<int>(VSyncMode::Mailbox),
                 Tr("The screen always gets the newest picture. No tearing and less delay, for more work. Not every computer can do it.")),
        Numbered(Tr("Off"), static_cast<int>(VSyncMode::Immediate), Tr("Pictures are shown as soon as they are drawn. The least delay, but the picture can tear.")),
    };
}

template <size_t Count>
Choices Sorted(const char * const (&names)[Count], bool translate = true)
{
    Choices choices;
    for (size_t index = 0; index < Count; ++index)
    {
        choices.push_back({translate ? Tr(names[index]) : names[index], std::to_string(index), {}});
    }
    std::sort(choices.begin(), choices.end(), [](const Choice & a, const Choice & b) { return a.name < b.name; });
    return choices;
}
} // namespace

GeneralDialog::~GeneralDialog() = default;

void GeneralDialog::RefreshLanguage()
{
    if (!IsVisible())
    {
        return;
    }
    RefreshTranslation();
    FillLists();
    ShowValues();
    ShowRunning();
    m_changes.clear();
}

GeneralDialog::GeneralDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, Host host) :
    Dialog(window.Context(), assetDir / "general_dialog.rml", kSettingsNarrowBelow),
    m_window(window),
    m_session(session),
    m_settings(settings),
    m_host(std::move(host))
{
}

void GeneralDialog::OnOpen()
{
    FillLists();
    ShowValues();
    ShowRunning();
    m_changes.clear();
    UiEvents::Listen(Document(), "change", [this](Rml::Event & event) { m_changes.push_back(event.GetTargetElement()->GetId()); });
}

void GeneralDialog::OnUpdate()
{
    std::vector<std::string> changes;
    changes.swap(m_changes);
    for (const std::string & id : changes)
    {
        ApplyChange(id);
    }
    ShowRunning();
}

void GeneralDialog::OnCommand(const std::string & command)
{
    SettingsContext & settings = m_settings;
    if (m_settings.InheritCommand(command))
    {
        FillLists();
        m_changes.clear();
        Changed();
        return;
    }
    SettingsPage page;
    if (SettingsPageOf(command, page))
    {
        if (m_host.openPage(page))
        {
            OnCloseRequested();
        }
    }
    else if (command == "mode-docked" || command == "mode-handheld")
    {
        settings.SetInt(NXOsSetting::DockedMode, static_cast<int32_t>(command == "mode-docked" ? DockedMode::Docked : DockedMode::Handheld));
        Changed();
    }
    else if (command == "toggle-region")
    {
        settings.SetInt(NXOsSetting::Region, RegionChosen() ? kRegionOfLanguage : LanguageRegion());
        Changed();
    }
    else if (command == "toggle-interface-language" && !m_settings.IsGame())
    {
        InterfaceLanguage::SetChoice(InterfaceLanguage::Choice().empty() ? InterfaceLanguage::Code() : std::string());
        ShowValues();
    }
}

void GeneralDialog::OnCloseRequested()
{
    if (m_unsaved)
    {
        m_unsaved = false;
        if (!m_settings.IsGame()) m_session.FlushSettings();
    }
    Close();
}

void GeneralDialog::FillLists()
{
    SettingsContext & settings = m_settings;
    Choices choices;

    Fill(Select(Document(), "interface-theme"),
         {
             {Tr("System default"), "system", {}},
             {Tr("Light"), "light", {}},
             {Tr("Dark"), "dark", {}},
         },
         uiSettings.interfaceTheme);

    m_devices = m_window.Renderer().GraphicsDevices();
    for (size_t index = 0; index < m_devices.size(); ++index)
    {
        choices.push_back({m_devices[index].name, std::to_string(index), m_devices[index].integrated ? Tr("Built into the processor.") : Tr("A graphics card.")});
    }
    if (choices.empty())
    {
        choices.push_back({Tr("Not available"), "0", {}});
    }
    Fill(Select(Document(), "device"), choices, std::to_string(settings.GetInt(NXVideoSetting::VulkanDevice)));

    choices.clear();
    for (int scale = 0; scale < kResolutionScaleCount; ++scale)
    {
        const bool native = scale == static_cast<int>(ResolutionSetup::Res1X);
        const float factor = kResolutionScales[scale].factor;
        const std::string hint = native ? Tr("What the console draws.")
                                 : factor < 1.0f ? Tr("Less than the console draws: blurrier, for a computer that struggles.")
                                                 : Tr("More than the console draws: sharper, and more work for the graphics device.");
        choices.push_back({native ? Tr("%1 (native)", {kResolutionScales[scale].label}) : std::string(kResolutionScales[scale].label), std::to_string(scale), hint});
    }
    Fill(Select(Document(), "resolution"), choices, std::to_string(settings.GetInt(NXVideoSetting::ResolutionSetup)));
    choices.clear();
    for (const ScalingFilterChoice & filter : kScalingFilters)
    {
        choices.push_back(Numbered(Tr(filter.label), static_cast<int>(filter.value), Tr(filter.hint)));
    }
    if (FindScalingFilter(settings.GetInt(NXVideoSetting::ScalingFilter)) == nullptr)
    {
        choices.push_back(Numbered(Tr("Gaussian"), settings.GetInt(NXVideoSetting::ScalingFilter), {}));
    }
    Fill(Select(Document(), "filter"), choices, std::to_string(settings.GetInt(NXVideoSetting::ScalingFilter)));
    Fill(Select(Document(), "aspect"), Aspects(), std::to_string(settings.GetInt(NXVideoSetting::AspectRatio)));
    Fill(Select(Document(), "vsync"), VSyncs(), std::to_string(settings.GetInt(NXVideoSetting::VSyncMode)));

    choices.clear();
    if (m_session.IsValid())
    {
        for (const UserProfile & profile : UserProfiles::List(m_session.Modules().OperatingSystem()))
        {
            choices.push_back({profile.name, std::to_string(choices.size()), {}});
        }
    }
    m_profileCount = choices.size();
    Fill(Select(Document(), "profile"), choices, std::to_string(UserProfiles::Active(m_profileCount)));

    Fill(Select(Document(), "language"), Sorted(Translation::ConsoleLanguages, false), std::to_string(settings.GetInt(NXCoreSetting::LanguageIndex)));
    Fill(Select(Document(), "region"), Sorted(kRegions), {});
    choices.clear();
    for (const Translation::LanguageInfo & language : Translation::Languages)
    {
        choices.push_back({language.name, language.code, {}});
    }
    Fill(Select(Document(), "interface-language"), choices, InterfaceLanguage::Code());
}

void GeneralDialog::ShowValues()
{
    SettingsContext & settings = m_settings;
    const bool docked = settings.GetInt(NXOsSetting::DockedMode) == static_cast<int32_t>(DockedMode::Docked);
    SetClass(Document(), "mode-docked", "selected", docked);
    SetClass(Document(), "mode-handheld", "selected", !docked);
    SetText("mode-text", docked ? Tr("Docked: games run at 1080p and expect a TV.") : Tr("Handheld: games run at 720p, as on the console's own screen."));

    const int scale = std::clamp(settings.GetInt(NXVideoSetting::ResolutionSetup), 0, kResolutionScaleCount - 1);
    const int height = static_cast<int>(std::lround((docked ? kDockedHeight : kHandheldHeight) * kResolutionScales[scale].factor));
    SetText("resolution-text", Tr("Games are drawn in %1p.", {std::to_string(height)}));

    const bool regionChosen = RegionChosen();
    Choose(Select(Document(), "region"), std::to_string(regionChosen ? settings.GetInt(NXOsSetting::Region) : LanguageRegion()));
    SetDisabled(Document(), "region", !regionChosen);
    SetClass(Document(), "region-chosen", "on", regionChosen);
    SetText("region-text", regionChosen ? Tr("Chosen here, whatever the language.") : Tr("Follows the console language."));

    const bool interfaceChosen = !InterfaceLanguage::Choice().empty();
    Choose(Select(Document(), "interface-language"), InterfaceLanguage::Code());
    SetDisabled(Document(), "interface-language", !interfaceChosen);
    SetClass(Document(), "interface-language-chosen", "on", interfaceChosen);
    SetText("interface-language-text", interfaceChosen ? Tr("Chosen here, whatever the language.") : Tr("Follows the console language."));

    const int sharpness = std::clamp(settings.GetInt(NXVideoSetting::FSPSharpness), 0, kSharpnessMax);
    SetClass(Document(), "sharpness", "absent", settings.GetInt(NXVideoSetting::ScalingFilter) != static_cast<int>(ScalingFilter::Fsr));
    SetRange(Document(), "sharpness-range", sharpness);
    SetText("sharpness-text", std::to_string(sharpness) + "%");

    ShowDevice();
    m_settings.Decorate(Document());
}

void GeneralDialog::ShowDevice()
{
    const int chosen = m_settings.GetInt(NXVideoSetting::VulkanDevice);
    std::string note;
    if (chosen >= 0 && chosen < static_cast<int>(m_devices.size()))
    {
        const auto faster = std::find_if(m_devices.begin(), m_devices.end(), [](const UiRenderer::GraphicsDevice & device) { return !device.integrated; });
        if (m_devices[chosen].integrated && faster != m_devices.end())
        {
            note = Tr("This is the graphics built into the processor. Games run better on %1.", {Rml::StringUtilities::EncodeRml(faster->name)});
        }
        else if (!m_devices[chosen].inUse)
        {
            note = Tr("Used from the next game on.");
        }
    }
    SetText("device-text", m_devices.size() > 1 ? Tr("What draws the games: this computer has %1.", {std::to_string(m_devices.size())}) : Tr("What draws the games."));
    SetText("device-note-text", note);
    SetClass(Document(), "device-note", "shown", !note.empty());
}

void GeneralDialog::ShowRunning()
{
    const bool running = m_session.IsRunning() && !m_settings.IsGame();
    Document()->SetClass("game-running", running);
    SetDisabled(Document(), "device", running || m_devices.empty());
    SetDisabled(Document(), "resolution", running);
    SetDisabled(Document(), "profile", running || m_profileCount < 2);
}

void GeneralDialog::ApplyChange(const std::string & id)
{
    SettingsContext & settings = m_settings;
    Rml::Element * element = Document()->GetElementById(id);
    std::string text;
    if (auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(element))
    {
        text = select->GetValue();
    }
    else if (auto * input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(element))
    {
        text = input->GetValue();
    }
    else
    {
        return;
    }
    if (m_settings.IsGame() && (id == "device" || id == "profile" || id == "interface-language" || id == "interface-theme")) return;
    const int value = std::atoi(text.c_str());

    if (id == "interface-theme")
    {
        if ((text == "system" || text == "light" || text == "dark") && text != uiSettings.interfaceTheme)
        {
            uiSettings.interfaceTheme = text;
            SaveUISetting();
            m_window.SetTheme(text);
        }
        return;
    }

    if (id == "interface-language")
    {
        if (!InterfaceLanguage::Choice().empty() && Translation::IsSupportedLanguage(text))
        {
            InterfaceLanguage::SetChoice(text);
            ShowValues();
        }
        return;
    }

    const char * setting = nullptr;
    if (id == "profile")
    {
        if (!m_session.IsRunning() && value >= 0 && static_cast<size_t>(value) < m_profileCount && value != UserProfiles::Active(m_profileCount))
        {
            UserProfiles::SetActive(value);
            Changed();
        }
        return;
    }
    if (id == "device" || id == "resolution")
    {
        if ((m_session.IsRunning() && !m_settings.IsGame()) || (id == "device" && m_devices.empty()))
        {
            return;
        }
        setting = id == "device" ? NXVideoSetting::VulkanDevice : NXVideoSetting::ResolutionSetup;
    }
    else if (id == "filter")
    {
        setting = NXVideoSetting::ScalingFilter;
    }
    else if (id == "sharpness-range")
    {
        setting = NXVideoSetting::FSPSharpness;
    }
    else if (id == "aspect")
    {
        setting = NXVideoSetting::AspectRatio;
    }
    else if (id == "vsync")
    {
        setting = NXVideoSetting::VSyncMode;
    }
    else if (id == "language")
    {
        setting = NXCoreSetting::LanguageIndex;
    }
    else if (id == "region")
    {
        if (!RegionChosen())
        {
            return;
        }
        setting = NXOsSetting::Region;
    }
    if (setting != nullptr && settings.GetInt(setting) != value)
    {
        settings.SetInt(setting, value);
        Changed();
    }
}

bool GeneralDialog::RegionChosen() const
{
    const int region = m_settings.GetInt(NXOsSetting::Region);
    return region >= 0 && region < static_cast<int>(std::size(kRegions));
}

int GeneralDialog::LanguageRegion() const
{
    const int language = std::clamp(m_settings.GetInt(NXCoreSetting::LanguageIndex), 0, static_cast<int>(std::size(Translation::ConsoleLanguages)) - 1);
    return static_cast<int>(RegionOfLanguage(static_cast<Language>(language)));
}

void GeneralDialog::Changed()
{
    m_unsaved = true;
    ShowValues();
    m_host.changed();
}
