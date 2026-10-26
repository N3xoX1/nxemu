#include "settings/ui_identifiers.h"
#include "ui_activity.h"
#include "ui_host.h"
#include "ui_util.h"

#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <RmlUi/Core/ElementDocument.h>

namespace
{
    constexpr float kResolutionScales[] = {0.5f, 0.75f, 1, 1.5f, 2, 3, 4, 5, 6, 7, 8};
    constexpr const char * kDot = " \xC2\xB7 ";

    enum Button
    {
        A = 0, B = 1, X = 2, Y = 3, LStick = 4, RStick = 5, L = 6, R = 7, ZL = 8, ZR = 9, Plus = 10, Minus = 11,
        DLeft = 12, DUp = 13, DRight = 14, DDown = 15, SLLeft = 16, SRLeft = 17, Home = 18, Capture = 19, SLRight = 20, SRRight = 21,
    };
    enum Stick
    {
        LeftStick = 0,
        RightStick = 1,
    };
    enum Style
    {
        StyleNone = 0,
        StylePro = 3,
        StyleHandheld = 4,
        StyleDualJoycons = 5,
        StyleLeftJoycon = 6,
        StyleRightJoycon = 7,
        StyleGameCube = 8,
    };

    std::string ControllerStyleName(int style)
    {
        switch (style)
        {
        case StylePro: return "Pro Controller";
        case StyleHandheld: return "Handheld";
        case StyleDualJoycons: return "Dual Joycons";
        case StyleLeftJoycon: return "Left Joycon";
        case StyleRightJoycon: return "Right Joycon";
        case StyleGameCube: return "GameCube Controller";
        case StyleNone: return "None";
        default: return "Unknown";
        }
    }

    bool GameIsRunning()
    {
        return UiUtil::ReadBool(NXCoreSetting::EmulationRunning);
    }

    bool Docked()
    {
        return UiUtil::ReadInt(NXOsSetting::DockedMode) == 1;
    }

    int ThemeMode()
    {
        const int mode = UiUtil::ReadInt(NXUISetting::ThemeMode);
        return mode == 1 || mode == 2 ? mode : 0;
    }

    std::string ThemeModeLabel(int mode)
    {
        return mode == 1 ? "Light" : mode == 2 ? "Dark" : "Follow system";
    }

    std::string ResolutionHeight(int value, bool docked)
    {
        const float scale = value >= 0 && value < static_cast<int>(std::size(kResolutionScales)) ? kResolutionScales[value] : 1.0f;
        return std::to_string(static_cast<int>(std::lround((docked ? 1080 : 720) * scale))) + "p";
    }

    bool ResolutionKnown()
    {
        const int value = UiUtil::ReadInt(NXVideoSetting::ResolutionSetup);
        return value >= 0 && value < static_cast<int>(std::size(kResolutionScales));
    }

    std::string ConsoleSummary()
    {
        const std::string docked = Docked() ? "Docked" : "Handheld";
        if (!UiUtil::ReadBool(NXOsSetting::UseSpeedLimit))
        {
            return docked + kDot + "Unlimited speed";
        }
        const int limit = UiUtil::ReadInt(NXOsSetting::SpeedLimit);
        return limit == 100 ? docked : docked + kDot + "Speed limit " + std::to_string(limit) + "%";
    }

    std::string FormatBytes(int64_t size)
    {
        if (size <= 0)
        {
            return {};
        }
        if (size < 1024 * 1024)
        {
            return std::to_string(std::max<int64_t>(1, std::llround(size / 1024.0))) + " KB";
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", size / (1024.0 * 1024.0));
        return text;
    }

    std::string FormatReleaseDate(const std::string & text)
    {
        static constexpr const char * kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int year = 0, month = 0, day = 0;
        if (std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || month < 1 || month > 12 || day < 1)
        {
            return {};
        }
        return std::string(kMonths[month - 1]) + " " + std::to_string(day) + ", " + std::to_string(year);
    }

    std::string Join(const std::vector<std::string> & parts, const char * separator)
    {
        std::string text;
        for (const std::string & part : parts)
        {
            if (part.empty())
            {
                continue;
            }
            if (!text.empty())
            {
                text += separator;
            }
            text += part;
        }
        return text;
    }

    std::string Sub(const std::string & detail, const char * id = nullptr)
    {
        if (detail.empty() && id == nullptr)
        {
            return {};
        }
        std::string attributes = id != nullptr ? std::string(" id=\"") + id + "\"" : std::string();
        return "<div class=\"setting-sub\"" + attributes + ">" + UiUtil::Escape(detail) + "</div>";
    }

    std::string Info(const std::string & label, const std::string & detail)
    {
        return "<div class=\"setting-info\"><div class=\"setting-label\">" + UiUtil::Escape(label) + "</div>" + Sub(detail) + "</div>";
    }

    constexpr const char * kChevron = "<img class=\"setting-chevron\" src=\"images/chevron-right.png\"/>";

    std::string Check(bool on)
    {
        return std::string("<img class=\"setting-check") + (on ? " on" : "") + "\" src=\"images/check.png\"/>";
    }
}

std::string UiActivity::Action(std::function<void()> action)
{
    m_actions.push_back(std::move(action));
    return " data-action=\"" + std::to_string(m_actions.size() - 1) + "\"";
}

void UiActivity::BeginBody()
{
    m_body.clear();
    m_actions.clear();
    m_sliders.clear();
    m_fabAction = nullptr;
}

void UiActivity::CommitBody()
{
    m_building = true;
    m_settingsBody->SetInnerRML(std::string("<div class=\"settings-list") + (m_fabAction ? " with-fab" : "") + "\">" + m_body + "</div>");
    m_building = false;
    m_fab->SetClass("hidden", !m_fabAction);
}

void UiActivity::ShowFab(std::function<void()> action)
{
    m_fabAction = std::move(action);
}

void UiActivity::AppendHeader(const std::string & text)
{
    m_body += "<div class=\"settings-header\">" + UiUtil::Escape(text) + "</div>";
}

void UiActivity::AppendSub(const std::string & text)
{
    m_body += Sub(text);
}

void UiActivity::AppendLink(const std::string & label, const std::string & detail, std::function<void()> action)
{
    m_body += "<div class=\"settings-item nav\"" + Action(std::move(action)) + ">" + Info(label, detail) + kChevron + "</div>";
}

void UiActivity::AppendBool(const std::string & label, const std::string & detail, bool on, std::function<void()> action)
{
    m_body += "<div class=\"settings-item nav\"" + Action(std::move(action)) + ">" + Info(label, detail) + "<div class=\"settings-toggle" +
              (on ? " on" : "") + "\"><div class=\"toggle-knob\"/></div></div>";
}

void UiActivity::AppendSlider(const std::string & label, const std::string & valueText, float min, float max, float step, float value,
                              std::function<std::string(float)> changed)
{
    const std::string index = std::to_string(m_sliders.size());
    m_sliders.push_back(std::move(changed));
    m_body += "<div class=\"settings-item static nav\"><div class=\"setting-info\"><div class=\"setting-label\">" + UiUtil::Escape(label) +
              " <span class=\"slider-value\" id=\"slider-value-" + index + "\">" + UiUtil::Escape(valueText) + "</span></div>" +
              "<input type=\"range\" class=\"range\" min=\"" + std::to_string(min) + "\" max=\"" + std::to_string(max) + "\" step=\"" +
              std::to_string(step) + "\" value=\"" + std::to_string(value) + "\" data-slider=\"" + index + "\"/></div></div>";
}

void UiActivity::AppendCheck(const std::string & label, const std::string & detail, bool on, std::function<void()> action)
{
    m_body += "<div class=\"settings-item nav\"" + Action(std::move(action)) + ">" + Info(label, detail) + Check(on) + "</div>";
}

void UiActivity::AppendEmptyState(const std::string & title, const std::string & copy, const std::string & button, std::function<void()> action)
{
    m_body += "<div class=\"empty-state\"><p class=\"empty-title\">" + UiUtil::Escape(title) + "</p><p class=\"empty-copy\">" + UiUtil::Escape(copy) + "</p>";
    if (!button.empty())
    {
        m_body += "<div class=\"primary-btn nav\"" + Action(std::move(action)) + ">" + UiUtil::Escape(button) + "</div>";
    }
    m_body += "</div>";
}

void UiActivity::AppendEmptyHint(const std::string & text)
{
    m_body += "<div class=\"empty-hint\">" + UiUtil::Escape(text) + "</div>";
}

void UiActivity::AppendBoolSetting(const char * label, const char * key, const char * detail, bool intBool, bool save)
{
    const bool on = intBool ? UiUtil::ReadInt(key) == 1 : UiUtil::ReadBool(key);
    AppendBool(label, detail != nullptr ? detail : "", on, [this, key, intBool, save] {
        if (intBool)
        {
            UiUtil::SetInt(key, UiUtil::ReadInt(key) == 1 ? 0 : 1);
        }
        else
        {
            UiUtil::SetBool(key, !UiUtil::ReadBool(key));
        }
        if (save)
        {
            UiUtil::SaveSettings();
        }
        RenderCurrentSettings();
    });
}

void UiActivity::AppendIntSetting(const char * label, const char * key, int min, int max, const char * suffix)
{
    const int value = UiUtil::ReadInt(key);
    AppendSlider(label, std::to_string(value) + suffix, static_cast<float>(min), static_cast<float>(max), 1, static_cast<float>(value),
                 [key, suffix](float changed) {
                     const int next = static_cast<int>(std::lround(changed));
                     UiUtil::SetInt(key, next);
                     UiUtil::SaveSettings();
                     return std::to_string(next) + suffix;
                 });
}

void UiActivity::AppendSelectSetting(const Select & select)
{
    std::string detail;
    if (select.format)
    {
        detail = select.format();
    }
    else
    {
        const int value = UiUtil::ReadInt(select.key);
        detail = std::to_string(value);
        for (const Option & option : select.options)
        {
            if (option.value == value)
            {
                detail = option.label;
            }
        }
    }
    AppendLink(select.label, detail, [this, &select] {
        m_settingChoice = &select;
        Navigate("settingChoices");
    });
}

void UiActivity::BuildSelects()
{
    m_selects = {
        {"Speed limit", NXOsSetting::SpeedLimit,
         {{"25%", 25, ""}, {"50%", 50, "Half speed"}, {"75%", 75, ""}, {"100%", 100, "Normal speed"}, {"125%", 125, ""}, {"150%", 150, ""},
          {"200%", 200, "Double speed"}, {"300%", 300, ""}, {"400%", 400, ""}},
         [] { return std::to_string(UiUtil::ReadInt(NXOsSetting::SpeedLimit)) + "%"; },
         nullptr},
        {"Accuracy", NXVideoSetting::AccuracyLevel,
         {{"Normal", 0, "Fastest. Works well for most games"},
          {"High", 1, "Fixes some graphical glitches. Slower"},
          {"Extreme", 2, "Most accurate. Much slower, for troubleshooting"}},
         nullptr, nullptr},
        {"Resolution", NXVideoSetting::ResolutionSetup,
         {{"0.5x", 0, ""}, {"0.75x", 1, ""}, {"1x", 2, ""}, {"1.5x", 3, ""}, {"2x", 4, ""}, {"3x", 5, ""}, {"4x", 6, ""}, {"5x", 7, ""},
          {"6x", 8, ""}, {"7x", 9, ""}, {"8x", 10, ""}},
         [this] {
             if (!ResolutionKnown())
             {
                 return std::string();
             }
             const int value = UiUtil::ReadInt(NXVideoSetting::ResolutionSetup);
             const bool docked = Docked();
             return std::string(SelectFor(NXVideoSetting::ResolutionSetup).options[value].label) + kDot + ResolutionHeight(value, docked) +
                    (docked ? " docked" : " handheld");
         },
         [](const Option & option) { return ResolutionHeight(option.value, false) + " handheld" + kDot + ResolutionHeight(option.value, true) + " docked"; }},
        {"VSync", NXVideoSetting::VSyncMode,
         {{"Immediate", 0, "Lowest latency. The picture may tear"},
          {"Mailbox", 1, "Low latency without tearing. Uses more power"},
          {"FIFO", 2, "No tearing. Recommended"},
          {"FIFO Relaxed", 3, "Like FIFO, but tears briefly instead of stuttering when a frame is late"}},
         nullptr, nullptr},
        {"Window adapting filter", NXVideoSetting::ScalingFilter,
         {{"Nearest neighbor", 0, "Crisp pixels. Can look blocky"},
          {"Bilinear", 1, "Smooth and slightly soft"},
          {"Bicubic", 2, "Smooth and a little sharper than bilinear"},
          {"Gaussian", 3, "Very smooth and soft"},
          {"ScaleForce", 4, "Sharp upscaling, good for low resolutions"},
          {"FSR", 5, "AMD FidelityFX upscaling. Sharpest, costs a little performance"}},
         nullptr, nullptr},
        {"Anti-aliasing", NXVideoSetting::AntiAliasing,
         {{"None", 0, "Fastest"}, {"FXAA", 1, "Smooths jagged edges cheaply. Slightly blurry"}, {"SMAA", 2, "Smoother edges than FXAA. Costs a little more"}},
         nullptr, nullptr},
        {"Anisotropic filtering", NXVideoSetting::AnisotropicFiltering,
         {{"Automatic", 0, "Picks a level to match the resolution"},
          {"Default", 1, "What the game asks for. Fastest"},
          {"2x", 2, "Slightly sharper textures on floors and walls"},
          {"4x", 3, "Sharper textures on floors and walls"},
          {"8x", 4, "Sharper still. Costs more on slower phones"},
          {"16x", 5, "Sharpest. Costs the most"}},
         nullptr, nullptr},
        {"Aspect ratio", NXVideoSetting::AspectRatio,
         {{"16:9", 0, "Same as the Switch. Recommended"},
          {"4:3", 1, "Stretches the picture to 4:3"},
          {"21:9", 2, "Stretches the picture to ultrawide"},
          {"16:10", 3, "Stretches the picture to 16:10"},
          {"Stretch", 4, "Fills the whole screen"}},
         nullptr, nullptr},
    };
}

const UiActivity::Select & UiActivity::SelectFor(const char * key) const
{
    for (const Select & select : m_selects)
    {
        if (std::string_view(select.key) == key)
        {
            return select;
        }
    }
    return m_selects.front();
}

void UiActivity::RenderSettingsHome()
{
    BeginBody();
    int connected = 0;
    for (int player = 0; player < 8; player++)
    {
        connected += m_host.RequestBool("isControllerConnected", {std::to_string(player)}) ? 1 : 0;
    }
    AppendLink("Controllers", connected == 0 ? "None connected" : std::to_string(connected) + " connected", [this] { Navigate("controllerSettings"); });
    AppendLink("On screen controller", UiUtil::ReadBool(NXUISetting::ShowInputOverlay) ? "Shown" : "Hidden", [this] { Navigate("overlaySettings"); });
    AppendLink("Console", ConsoleSummary(), [this] { Navigate("systemSettings"); });
    std::string graphics;
    if (ResolutionKnown())
    {
        const int value = UiUtil::ReadInt(NXVideoSetting::ResolutionSetup);
        graphics = std::string("Resolution ") + SelectFor(NXVideoSetting::ResolutionSetup).options[value].label + " (" + ResolutionHeight(value, Docked()) + ")";
    }
    AppendLink("Graphics", graphics, [this] { Navigate("graphicsSettings"); });
    CommitBody();
}

void UiActivity::RenderHub()
{
    BeginBody();
    AppendLink("Settings", "Controllers, console, graphics", [this] { Navigate("settingsPage"); });
    AppendLink("Interface", ThemeModeLabel(ThemeMode()) + " theme", [this] { Navigate("interfaceSettings"); });
    const size_t folders = UiUtil::ConfiguredFolders().size();
    AppendLink("Game folders", folders == 0 ? "None added" : folders == 1 ? "1 folder" : std::to_string(folders) + " folders",
               [this] { Navigate("gameFolders"); });
    if (!GameIsRunning())
    {
        const std::string firmware = m_host.Request("firmwareVersion");
        AppendLink("Firmware", firmware.empty() ? "Not installed" : firmware, [this] { m_host.Request("installFirmware"); });
    }
    AppendLink("About", VER_FILE_VERSION_STR, [this] { Navigate("about"); });
    CommitBody();
}

void UiActivity::RenderAbout()
{
    BeginBody();
    m_body += "<div class=\"about\"><div class=\"about-top\"><img class=\"about-logo\" src=\"images/nxemu-logo.png\"/><div class=\"about-product\">"
              "<div class=\"about-name\">NXEmu</div><div class=\"about-muted\">" VER_FILE_VERSION_STR "</div>"
              "<div class=\"about-muted about-small\">" VER_COPYRIGHT_STR "</div></div></div>"
              "<p class=\"about-text\">NXEmu is an experimental open-source Nintendo Switch emulator.</p>"
              "<p class=\"about-text about-muted\">You are responsible for ensuring your use of this software complies with the laws in your jurisdiction.</p>"
              "<p class=\"about-credits\">With thanks to</p>"
              "<p class=\"about-text about-muted\">the yuzu project, Fabrice, Alexwpi, Stardust, and NullVector</p></div>";
    AppendLink("Website", "www.nxemu.com", [this] { m_host.Request("openUrl", {"https://www.nxemu.com/"}); });
    AppendLink("Source code", "github.com/N3xoX1/nxemu", [this] { m_host.Request("openUrl", {"https://github.com/N3xoX1/nxemu"}); });
    AppendLink("Discord", "discord.gg/hEa4hNyFWU", [this] { m_host.Request("openUrl", {"https://discord.gg/hEa4hNyFWU"}); });
    m_body += "<p class=\"about-trademark\">\"Nintendo Switch\" is a trademark of Nintendo. NXEmu is not affiliated with Nintendo in any way.</p>";
    CommitBody();
}

void UiActivity::RenderInterfaceSettings()
{
    BeginBody();
    AppendLink("Theme", ThemeModeLabel(ThemeMode()), [this] { Navigate("themeSettings"); });
    CommitBody();
}

void UiActivity::RenderOverlaySettings()
{
    BeginBody();
    AppendBoolSetting("Show overlay", NXUISetting::ShowInputOverlay, nullptr);
    AppendIntSetting("Scale", NXUISetting::OverlayScale, 0, 150, "%");
    AppendIntSetting("Opacity", NXUISetting::OverlayOpacity, 0, 100, "%");
    AppendLink("Edit overlay layout", "Drag buttons to reposition", [this] { m_host.Request("editOverlayLayout"); });
    CommitBody();
}

void UiActivity::RenderSystemSettings()
{
    BeginBody();
    if (GameIsRunning())
    {
        AppendBoolSetting("Limit speed", NXOsSetting::UseSpeedLimit, "Off runs as fast as possible. This session only", false, false);
    }
    AppendSelectSetting(SelectFor(NXOsSetting::SpeedLimit));
    AppendBoolSetting("Docked mode", NXOsSetting::DockedMode, "Higher resolution, lower performance", true);
    CommitBody();
}

void UiActivity::RenderGraphicsSettings()
{
    BeginBody();
    const bool running = GameIsRunning();
    if (running)
    {
        AppendHeader("Close the game to change resolution, the GPU driver and other options that apply when a game starts.");
    }
    if (!running && GpuDriverSupported())
    {
        AppendLink("GPU driver", GpuDriverLabel(), [this] { Navigate("gpuDrivers"); });
    }
    AppendSelectSetting(SelectFor(NXVideoSetting::AccuracyLevel));
    if (!running)
    {
        AppendSelectSetting(SelectFor(NXVideoSetting::ResolutionSetup));
    }
    AppendSelectSetting(SelectFor(NXVideoSetting::VSyncMode));
    AppendSelectSetting(SelectFor(NXVideoSetting::ScalingFilter));
    if (UiUtil::ReadInt(NXVideoSetting::ScalingFilter) == 5)
    {
        AppendIntSetting("FSR sharpness", NXVideoSetting::FSPSharpness, 0, 200, "%");
    }
    AppendSelectSetting(SelectFor(NXVideoSetting::AntiAliasing));
    AppendSelectSetting(SelectFor(NXVideoSetting::AnisotropicFiltering));
    AppendSelectSetting(SelectFor(NXVideoSetting::AspectRatio));
    if (!running)
    {
        AppendBoolSetting("Force maximum clocks", NXVideoSetting::ForceMaximumClocks, "Adreno GPUs. Can overheat the device");
        AppendBoolSetting("Asynchronous shaders", NXVideoSetting::UseAsynchronousShaderBuilding, "Reduces stutter, but can glitch");
        AppendBoolSetting("Reactive flushing", NXVideoSetting::EnableReactiveFlushing, "More accurate, costs performance");
        AppendBoolSetting("Disk shader cache", NXVideoSetting::UseDiskPipelineCache, "Games load faster after the first run");
    }
    CommitBody();
}

void UiActivity::RenderThemeSettings()
{
    BeginBody();
    for (int mode = 0; mode < 3; mode++)
    {
        AppendCheck(ThemeModeLabel(mode), "", ThemeMode() == mode, [this, mode] {
            UiUtil::SetInt(NXUISetting::ThemeMode, mode);
            UiUtil::SaveSettings();
            ApplyTheme();
            RenderCurrentSettings();
        });
    }
    CommitBody();
}

void UiActivity::RenderSettingChoices()
{
    BeginBody();
    if (m_settingChoice != nullptr)
    {
        const Select & select = *m_settingChoice;
        const int current = UiUtil::ReadInt(select.key);
        for (const Option & option : select.options)
        {
            const std::string detail = select.optionDetail ? select.optionDetail(option) : option.detail;
            AppendCheck(option.label, detail, current == option.value, [this, key = select.key, value = option.value] {
                UiUtil::SetInt(key, value);
                UiUtil::SaveSettings();
                GoBack();
            });
        }
    }
    CommitBody();
}

void UiActivity::RenderGameFolders()
{
    BeginBody();
    const std::vector<std::string> folders = UiUtil::ConfiguredFolders();
    if (folders.empty())
    {
        AppendEmptyState("No folders yet", "Choose a folder that contains .nro, .dxci, or .dnsp files.", "Add game folder",
                         [this] { m_host.Request("addGameDirectory"); });
    }
    for (const std::string & uri : folders)
    {
        m_body += "<div class=\"folder-item nav\"><div class=\"setting-info\"><div class=\"setting-label\">" + UiUtil::Escape(UiUtil::FormatGameFolder(uri)) +
                  "</div></div><div class=\"folder-remove\"" + Action([this, uri] { RemoveGameFolder(uri); }) +
                  "><img src=\"images/trash.png\"/></div></div>";
    }
    if (!folders.empty())
    {
        ShowFab([this] { m_host.Request("addGameDirectory"); });
    }
    CommitBody();
}

void UiActivity::RemoveGameFolder(const std::string & uri)
{
    Confirm("Remove this folder from the library? The files will not be deleted.", [uri] {
        JsonValue folders(JsonValueType::Array);
        for (const std::string & folder : UiUtil::ConfiguredFolders())
        {
            if (folder != uri)
            {
                folders[folders.size()] = folder;
            }
        }
        UiUtil::SetString(NXUISetting::GameDirectories, JsonStyledWriter().write(folders));
        UiUtil::SaveSettings();
    });
}

std::string UiActivity::SetupTitle() const
{
    const std::string & step = m_setupStep < m_setupSteps.size() ? m_setupSteps[m_setupStep] : std::string();
    if (step == "folders") return "Welcome to NxEmu";
    if (step == "controls") return "Controls";
    return "GPU driver";
}
void UiActivity::SetupNext()
{
    if (m_setupStep + 1 >= m_setupSteps.size())
    {
        FinishSetup();
        return;
    }
    m_setupStep++;
    m_settingsSelection = 0;
    RenderCurrentSettings();
}

void UiActivity::FinishSetup()
{
    m_host.Request("completeSetup");
    m_navStack = {"gamesPage"};
    m_settingsSelection = -1;
    ShowPage("gamesPage");
    ApplyControllerBrowser();
}

void UiActivity::AppendSetupIntro(const std::string & copy)
{
    m_body += "<div class=\"setup-intro\"><p class=\"setup-step\">Step " + std::to_string(m_setupStep + 1) + " of " +
              std::to_string(m_setupSteps.size()) + "</p><p class=\"setup-copy\">" + UiUtil::Escape(copy) + "</p></div>";
}

void UiActivity::AppendSetupNext(const std::string & label, bool done)
{
    m_body += std::string("<div class=\"setup-actions\"><div class=\"primary-btn nav") + (done ? " done-btn" : "") + "\"" +
              Action([this] { SetupNext(); }) + ">" + UiUtil::Escape(label) + "</div></div>";
}

void UiActivity::RenderSetup()
{
    BeginBody();
    const std::string & step = m_setupSteps[m_setupStep];
    const bool last = m_setupStep + 1 == m_setupSteps.size();
    if (step == "folders")
    {
        const std::vector<std::string> folders = UiUtil::ConfiguredFolders();
        AppendSetupIntro("Choose the folders that hold your games. NxEmu reads them where they are and never moves or copies your files.");
        for (const std::string & uri : folders)
        {
            m_body += "<div class=\"setup-folder\">" + UiUtil::Escape(UiUtil::FormatGameFolder(uri)) + "</div>";
        }
        AppendLink(folders.empty() ? "Add game folder" : "Add another folder", "Games in .nro, .dxci or .dnsp format",
                   [this] { m_host.Request("addGameDirectory"); });
        if (folders.empty())
        {
            AppendSetupNext("Skip", false);
        }
        else
        {
            AppendSetupNext(last ? "Done" : "Next", last);
        }
    }
    else if (step == "controls")
    {
        const std::vector<std::string> names = UiUtil::StringArray(UiUtil::ParseJson(m_host.Request("connectedControllerNames")));
        std::string list;
        for (const std::string & name : names)
        {
            list += (list.empty() ? "" : ", ") + name;
        }
        AppendSetupIntro(names.empty() ? "No controller needed. Touch controls appear on screen while you play. You can connect a Bluetooth or USB "
                                         "controller at any time."
                                       : "Controller connected: " + list + ". It's ready to play. Use A to select and B to go back.");
        AppendBoolSetting("Show on-screen controls", NXUISetting::ShowInputOverlay,
                          names.empty() ? "Turn off if you only play with a controller" : "Usually off when using a controller");
        if (!names.empty())
        {
            AppendLink("Controller settings", "Change button mapping", [this] { Navigate("controllerSettings"); });
        }
        AppendSetupNext(last ? "Done" : "Next", last);
    }
    else
    {
        if (!m_gpuCatalogReady && !m_gpuLoading)
        {
            LoadGpuCatalog(false);
        }
        const std::string gpu = m_gpuCatalogReady ? m_gpuCatalog["gpu"].asString() : std::string();
        AppendSetupIntro("Your phone has " + (gpu.empty() ? std::string("an Adreno GPU") : gpu) +
                         ". Many games run faster and with fewer glitches on a community driver. You can switch back to the system driver at "
                         "any time in Settings, Graphics.");
        if (!m_gpuCatalogReady)
        {
            AppendEmptyHint("Finding the best driver for your phone\xE2\x80\xA6");
        }
        else if (const JsonValue & rec = m_gpuCatalog["recommended"]; !rec["asset"].asString().empty())
        {
            AppendGpuAssetRow("Install " + rec["label"].asString(), rec["repo"].asString(), rec["tag"].asString(), rec["asset"].asString(),
                              rec["size"].asInt64(), rec["file"].asString(), StoredGpuDriverFiles(), rec["published"].asString());
        }
        else
        {
            const std::string error = m_gpuCatalog["error"].asString();
            m_body += "<div class=\"setup-folder\">" +
                    UiUtil::Escape(!error.empty() ? error
                                            : "The recommended driver isn't available right now. You can install one later from Settings, Graphics, GPU "
                                              "driver.") +
                      "</div>";
        }
        const bool active = GpuDriverLabel() != "System";
        AppendSetupNext(active ? "Done" : "Skip", active);
    }
    CommitBody();
}
JsonValue UiActivity::GpuDriverList() const
{
    return UiUtil::ParseJson(m_host.Request("gpuDriverList"));
}

bool UiActivity::GpuDriverSupported() const
{
    return GpuDriverList()["supported"].asBool();
}

std::string UiActivity::GpuDriverLabel() const
{
    const std::string active = GpuDriverList()["active"].asString();
    return active.empty() ? "System" : active;
}

JsonValue UiActivity::StoredGpuDriverFiles() const
{
    JsonValue files(JsonValueType::Object);
    const JsonValue drivers = GpuDriverList()["drivers"];
    for (uint32_t i = 0; i < drivers.size(); i++)
    {
        files[drivers[i]["file"].asString()] = drivers[i];
    }
    return files;
}

void UiActivity::RenderGpuDriversPage()
{
    const std::string & top = Top();
    if (top == "gpuDrivers" || top == "graphicsSettings" || top == "setup" || top == "gpuDriverDownload" || top == "gpuDriverGroup" ||
        top == "gpuDriverRelease")
    {
        RenderCurrentSettings();
    }
}

void UiActivity::LoadGpuCatalog(bool force)
{
    if (m_gpuLoading)
    {
        return;
    }
    m_gpuLoading = true;
    m_host.Request("fetchGpuDriverCatalog", {force ? "true" : "false"});
}

void UiActivity::OnGpuDriverCatalog(const JsonValue & catalog)
{
    m_gpuLoading = false;
    m_gpuCatalog = catalog;
    m_gpuCatalogReady = true;
    const std::string & top = Top();
    if (top == "gpuDriverDownload" || top == "gpuDriverGroup" || top == "gpuDriverRelease" || top == "setup" || top == "gpuDrivers")
    {
        RenderCurrentSettings();
    }
}

void UiActivity::OnGpuDriverDownload(const JsonValue & event)
{
    if (!m_gpuDownload)
    {
        return;
    }
    if (event.isMember("result"))
    {
        m_gpuDownload.reset();
    }
    else
    {
        const int percent = static_cast<int>(event["percent"].asInt64());
        if (m_gpuDownload->percent == percent)
        {
            return;
        }
        m_gpuDownload->percent = percent;
        if (Rml::Element * status = m_document->GetElementById("gpu-download-status"))
        {
            status->SetInnerRML("Downloading " + std::to_string(percent) + "%");
            return;
        }
    }
    RenderGpuDriversPage();
}

void UiActivity::AppendGpuAssetRow(const std::string & title, const std::string & repo, const std::string & tag, const std::string & asset,
                                   int64_t size, const std::string & file, const JsonValue & stored, const std::string & published)
{
    const bool downloading = m_gpuDownload && m_gpuDownload->repo == repo && m_gpuDownload->tag == tag && m_gpuDownload->asset == asset;
    const bool installed = stored.isMember(file);
    const bool selected = installed && stored[file]["selected"].asBool();
    const std::string released = published.empty() ? std::string() : FormatReleaseDate(published);
    std::string status = Join({FormatBytes(size), released.empty() ? std::string() : "Released " + released}, kDot);
    if (downloading)
    {
        status = "Downloading " + std::to_string(m_gpuDownload->percent) + "%";
    }
    else if (selected)
    {
        status = std::string("Installed") + kDot + "in use";
    }
    else if (installed)
    {
        status = std::string("Installed") + kDot + "tap to use";
    }
    const std::string icon = installed ? Check(selected)
                                       : std::string("<img class=\"setting-download") + (downloading ? " busy" : "") + "\" src=\"images/download.png\"/>";
    const std::string action = Action([this, installed, selected, file, repo, tag, asset] {
        if (m_gpuDownload)
        {
            return;
        }
        if (installed)
        {
            if (!selected)
            {
                m_host.Request("selectGpuDriver", {file});
            }
            return;
        }
        m_gpuDownload = std::make_unique<Download>(Download{repo, tag, asset, 0});
        m_host.Request("downloadGpuDriver", {repo, tag, asset});
        RenderCurrentSettings();
    });
    m_body += "<div class=\"settings-item nav\"" + action + "><div class=\"setting-info\"><div class=\"setting-label\">" + UiUtil::Escape(title) + "</div>" +
              Sub(status, downloading ? "gpu-download-status" : nullptr) + "</div>" + icon + "</div>";
}

bool UiActivity::RenderGpuCatalogState()
{
    if (!m_gpuCatalogReady && !m_gpuLoading)
    {
        LoadGpuCatalog(false);
    }
    if (!m_gpuCatalogReady)
    {
        AppendEmptyHint("Loading drivers from GitHub\xE2\x80\xA6");
        return false;
    }
    if (m_gpuCatalog.isMember("error") && !m_gpuCatalog["error"].asString().empty())
    {
        AppendEmptyState("Couldn't load drivers", m_gpuCatalog["error"].asString(), "Try again", [this] {
            m_gpuCatalogReady = false;
            LoadGpuCatalog(true);
            RenderCurrentSettings();
        });
        return false;
    }
    return true;
}

void UiActivity::RenderGpuDriverDownload()
{
    BeginBody();
    if (RenderGpuCatalogState())
    {
        const JsonValue stored = StoredGpuDriverFiles();
        const JsonValue & rec = m_gpuCatalog["recommended"];
        const std::string gpu = m_gpuCatalog["gpu"].asString();
        AppendHeader(gpu.empty() ? "Recommended" : "Recommended for " + gpu);
        if (!rec["asset"].asString().empty())
        {
            AppendGpuAssetRow(rec["label"].asString(), rec["repo"].asString(), rec["tag"].asString(), rec["asset"].asString(), rec["size"].asInt64(),
                              rec["file"].asString(), stored, rec["published"].asString());
        }
        else
        {
            AppendSub(rec.isObject() ? rec["label"].asString() + " isn't listed right now. Pick a driver below."
                                     : "No specific driver is recommended for this GPU. A recent Turnip build is a good start.");
        }
        AppendHeader("All drivers");
        const JsonValue & groups = m_gpuCatalog["groups"];
        for (uint32_t g = 0; g < groups.size(); g++)
        {
            const JsonValue & group = groups[g];
            const JsonValue & releases = group["releases"];
            uint32_t latest = 0;
            for (uint32_t r = 0; r < releases.size(); r++)
            {
                if (releases[r]["latest"].asBool())
                {
                    latest = r;
                    break;
                }
            }
            std::string detail = group["error"].asString();
            if (detail.empty())
            {
                detail = releases.size() > 0 ? "Latest: " + releases[latest]["title"].asString() + kDot + std::to_string(releases.size()) + " releases"
                                             : "No downloads";
            }
            if (group["stale"].asBool())
            {
                detail += std::string(kDot) + "offline copy";
            }
            AppendLink(group["name"].asString(), detail, [this, g, count = releases.size()] {
                if (count == 0)
                {
                    return;
                }
                m_gpuGroup = g;
                Navigate("gpuDriverGroup");
            });
        }
        AppendLink(m_gpuLoading ? "Refreshing\xE2\x80\xA6" : "Refresh list", "Check GitHub for new releases", [this] {
            LoadGpuCatalog(true);
            RenderCurrentSettings();
        });
    }
    CommitBody();
}

void UiActivity::RenderGpuDriverGroup()
{
    BeginBody();
    if (m_gpuCatalogReady)
    {
        const JsonValue stored = StoredGpuDriverFiles();
        const JsonValue & releases = m_gpuCatalog["groups"][m_gpuGroup]["releases"];
        for (uint32_t r = 0; r < releases.size(); r++)
        {
            const JsonValue & release = releases[r];
            std::vector<std::string> tags;
            if (release["latest"].asBool())
            {
                tags.push_back("Latest");
            }
            if (release["prerelease"].asBool())
            {
                tags.push_back("Pre-release");
            }
            tags.push_back(FormatReleaseDate(release["published"].asString()));
            const JsonValue & assets = release["assets"];
            for (uint32_t a = 0; a < assets.size(); a++)
            {
                if (stored.isMember(assets[a]["file"].asString()))
                {
                    tags.push_back("Installed");
                    break;
                }
            }
            AppendLink(release["title"].asString(), Join(tags, kDot), [this, r] {
                m_gpuRelease = r;
                Navigate("gpuDriverRelease");
            });
        }
    }
    CommitBody();
}

void UiActivity::RenderGpuDriverRelease()
{
    BeginBody();
    if (m_gpuCatalogReady)
    {
        const JsonValue stored = StoredGpuDriverFiles();
        const JsonValue & group = m_gpuCatalog["groups"][m_gpuGroup];
        const JsonValue & release = group["releases"][m_gpuRelease];
        const JsonValue & assets = release["assets"];
        AppendHeader(assets.size() > 1 ? std::string("Downloads") + kDot + "pick one" : "Download");
        for (uint32_t a = 0; a < assets.size(); a++)
        {
            const JsonValue & asset = assets[a];
            AppendGpuAssetRow(asset["name"].asString(), group["repo"].asString(), release["tag"].asString(), asset["name"].asString(),
                              asset["size"].asInt64(), asset["file"].asString(), stored, "");
        }
        std::string notes = release["notes"].asString();
        std::erase(notes, '\r');
        const size_t first = notes.find_first_not_of(" \n\t");
        const size_t last = notes.find_last_not_of(" \n\t");
        notes = first == std::string::npos ? std::string() : notes.substr(first, last - first + 1);
        if (!notes.empty())
        {
            AppendHeader("Release notes");
            m_body += "<div class=\"release-notes\">" + UiUtil::Escape(notes) + "</div>";
        }
    }
    CommitBody();
}

void UiActivity::AppendGpuRecommendation()
{
    if (!m_gpuCatalogReady)
    {
        if (!m_gpuLoading)
        {
            LoadGpuCatalog(false);
        }
        return;
    }
    const JsonValue & rec = m_gpuCatalog["recommended"];
    if (rec["asset"].asString().empty())
    {
        return;
    }
    const JsonValue stored = StoredGpuDriverFiles();
    if (stored.isMember(rec["file"].asString()))
    {
        return;
    }
    const std::string gpu = m_gpuCatalog["gpu"].asString();
    AppendHeader(gpu.empty() ? "Recommended" : "Recommended for " + gpu);
    AppendGpuAssetRow(rec["label"].asString(), rec["repo"].asString(), rec["tag"].asString(), rec["asset"].asString(), rec["size"].asInt64(),
                      rec["file"].asString(), stored, rec["published"].asString());
}

void UiActivity::AppendGpuDriverRow(const std::string & title, const std::string & subtitle, bool selected, std::function<void()> select,
                                    std::function<void()> remove)
{
    std::string removeButton;
    if (remove)
    {
        removeButton = "<div class=\"folder-remove\"" + Action(std::move(remove)) + "><img src=\"images/trash.png\"/></div>";
    }
    m_body += "<div class=\"settings-item nav\"" + Action(std::move(select)) + ">" + Info(title, subtitle) + removeButton + Check(selected) + "</div>";
}

void UiActivity::RenderGpuDrivers()
{
    BeginBody();
    const JsonValue info = GpuDriverList();
    if (!info["supported"].asBool())
    {
        AppendEmptyState("Adreno only", "Custom GPU drivers need an Adreno GPU. This device keeps using the system driver.", "", nullptr);
        CommitBody();
        return;
    }
    AppendHeader("Applies the next time a game starts.");
    const std::string active = info["active"].asString();
    AppendGpuDriverRow("System", "Device driver", active.empty() || active == "System", [this] { m_host.Request("selectGpuDriver", {""}); }, nullptr);
    const JsonValue & drivers = info["drivers"];
    for (uint32_t i = 0; i < drivers.size(); i++)
    {
        const std::string file = drivers[i]["file"].asString();
        const std::string name = drivers[i]["name"].asString();
        AppendGpuDriverRow(name, drivers[i]["version"].asString(), drivers[i]["selected"].asBool(),
                           [this, file] { m_host.Request("selectGpuDriver", {file}); },
                           [this, file, name] { Confirm("Remove " + name + "?", [this, file] { m_host.Request("removeGpuDriver", {file}); }); });
    }
    AppendGpuRecommendation();
    ShowFab([this] { Navigate("gpuDriverAdd"); });
    CommitBody();
}

void UiActivity::RenderGpuDriverAdd()
{
    BeginBody();
    AppendLink("Download", "Browse Turnip and other drivers on GitHub", [this] {
        m_navStack.pop_back();
        Navigate("gpuDriverDownload");
    });
    AppendLink("Install from file", "Choose a driver .zip already on this device", [this] {
        GoBack();
        m_host.Request("installGpuDriver");
    });
    CommitBody();
}

void UiActivity::RenderControllersSummary()
{
    ApplyControllerBrowser();
    if (Top() == "settingsPage" || Top() == "setup")
    {
        RenderCurrentSettings();
    }
}

void UiActivity::RenderControllerPages()
{
    if (Top() == "controllerSettings" || Top() == "controllerPlayer")
    {
        RenderCurrentSettings();
    }
}

void UiActivity::RenderControllerSettings()
{
    BeginBody();
    for (int player = 0; player < 8; player++)
    {
        const std::string index = std::to_string(player);
        const bool connected = m_host.RequestBool("isControllerConnected", {index});
        const std::string style = ControllerStyleName(m_host.RequestInt("getControllerStyle", {index}));
        AppendLink("Controller " + std::to_string(player + 1), connected ? "Connected - " + style : "Disconnected", [this, player] {
            m_controllerPlayer = player;
            m_controllerFilter = 0;
            Navigate("controllerPlayer");
        });
    }
    CommitBody();
}

void UiActivity::AppendControllerMap(const std::string & title, const std::string & mapId)
{
    const std::string player = std::to_string(m_controllerPlayer);
    std::string binding;
    if (mapId.starts_with("button:"))
    {
        binding = m_host.Request("getButtonBinding", {player, mapId.substr(7)});
    }
    else
    {
        // analog:<stick>:<direction> or modifier:<stick>
        const size_t first = mapId.find(':');
        const size_t second = mapId.find(':', first + 1);
        const std::string stick = mapId.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        const std::string part = mapId.starts_with("modifier") ? "modifier" : mapId.substr(second + 1);
        binding = m_host.Request("getStickBinding", {player, stick, part});
    }
    AppendLink(title, binding, [this, title, mapId] {
        m_host.Request("beginControllerMap", {std::to_string(m_controllerPlayer), mapId, title, std::to_string(m_controllerFilter)});
    });
}

void UiActivity::AppendStickSlider(const std::string & title, int stick, const char * key, float min, float max)
{
    const std::string player = std::to_string(m_controllerPlayer);
    const float value = std::strtof(m_host.Request("getStickValue", {player, std::to_string(stick), key}).c_str(), nullptr);
    const auto percent = [](float v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; };
    AppendSlider(title, percent(value), min, max, 0.01f, value, [this, stick, key, percent](float changed) {
        m_host.Request("setStickValue", {std::to_string(m_controllerPlayer), std::to_string(stick), key, std::to_string(changed)});
        return percent(changed);
    });
}

void UiActivity::AppendStickDirections(int stick)
{
    const std::string prefix = "analog:" + std::to_string(stick) + ":";
    AppendControllerMap("Up", prefix + "up");
    AppendControllerMap("Down", prefix + "down");
    AppendControllerMap("Left", prefix + "left");
    AppendControllerMap("Right", prefix + "right");
}

void UiActivity::AppendStickExtras(int stick)
{
    if (m_host.RequestBool("isControllerStick", {std::to_string(m_controllerPlayer), std::to_string(stick)}))
    {
        AppendStickSlider("Range", stick, "range", 0.25f, 1.5f);
        AppendStickSlider("Deadzone", stick, "deadzone", 0, 1);
    }
    else
    {
        AppendControllerMap("Modifier", "modifier:" + std::to_string(stick));
        AppendStickSlider("Modifier range", stick, "modifier_scale", 0, 1);
    }
}

void UiActivity::AppendButtons(std::initializer_list<std::pair<const char *, int>> buttons)
{
    for (const auto & [title, id] : buttons)
    {
        AppendControllerMap(title, "button:" + std::to_string(id));
    }
}

void UiActivity::RenderControllerPlayer()
{
    BeginBody();
    const int player = m_controllerPlayer;
    const std::string index = std::to_string(player);
    const int style = m_host.RequestInt("getControllerStyle", {index});
    const std::vector<std::string> filters = UiUtil::StringArray(UiUtil::ParseJson(m_host.Request("getControllerFilterNames")));
    if (m_controllerFilter >= static_cast<int>(filters.size()))
    {
        m_controllerFilter = 0;
    }

    AppendBool("Connected", "", m_host.RequestBool("isControllerConnected", {index}), [this, index] {
        m_host.Request("setControllerConnected", {index, m_host.RequestBool("isControllerConnected", {index}) ? "false" : "true"});
        RenderCurrentSettings();
    });
    AppendLink("Controller type", ControllerStyleName(style), [this, index] {
        const JsonValue styles = UiUtil::ParseJson(m_host.Request("getSupportedControllerStyles", {index}));
        Choices choices{"Controller type", {}, nullptr, ""};
        std::vector<int> ids;
        for (uint32_t i = 0; i < styles.size(); i++)
        {
            ids.push_back(static_cast<int>(styles[i].asInt64()));
            choices.items.push_back(ControllerStyleName(ids.back()));
        }
        choices.pick = [this, index, ids](size_t picked) {
            m_host.Request("setControllerStyle", {index, std::to_string(ids[picked])});
            GoBack();
        };
        OpenControllerChoices(std::move(choices));
    });
    AppendLink("Auto-map a controller", "Select a device to attempt auto-mapping", [this, index] {
        Choices choices{"Auto-map a controller", UiUtil::StringArray(UiUtil::ParseJson(m_host.Request("getAutoMapControllerNames"))), nullptr, "No controllers found"};
        choices.pick = [this, index, names = choices.items](size_t picked) {
            m_host.Request("autoMapController", {index, std::to_string(picked)});
            m_host.Request("alert", {"Ran auto-mapping against " + names[picked]});
            GoBack();
        };
        OpenControllerChoices(std::move(choices));
    });
    AppendLink("Input mapping filter", m_controllerFilter < static_cast<int>(filters.size()) ? filters[m_controllerFilter] : "Unknown", [this] {
        Choices choices{"Input mapping filter", UiUtil::StringArray(UiUtil::ParseJson(m_host.Request("getControllerFilterNames"))), nullptr, "No controllers found"};
        choices.pick = [this](size_t picked) {
            m_controllerFilter = static_cast<int>(picked);
            GoBack();
        };
        OpenControllerChoices(std::move(choices));
    });
    AppendLink("Reset to default", "", [this, index] {
        Confirm("Reset this player to defaults?", [this, index] {
            m_host.Request("resetControllerMappings", {index});
            RenderCurrentSettings();
        });
    });

    const bool full = style == StylePro || style == StyleHandheld || style == StyleDualJoycons;
    const bool left = style == StyleLeftJoycon;
    const bool right = style == StyleRightJoycon;
    const bool gamecube = style == StyleGameCube;
    if (full)
    {
        AppendHeader("Buttons");
        AppendButtons({{"A", A}, {"B", B}, {"X", X}, {"Y", Y}, {"Plus", Plus}, {"Minus", Minus}, {"Home", Home}, {"Capture", Capture}});
    }
    else if (left)
    {
        AppendHeader("Buttons");
        AppendButtons({{"Minus", Minus}, {"Capture", Capture}});
    }
    else if (right)
    {
        AppendHeader("Buttons");
        AppendButtons({{"A", A}, {"B", B}, {"X", X}, {"Y", Y}, {"Plus", Plus}, {"Home", Home}});
    }
    else if (gamecube)
    {
        AppendHeader("Buttons");
        AppendButtons({{"A", A}, {"B", B}, {"X", X}, {"Y", Y}, {"Start/Pause", Plus}});
    }

    if (full || left)
    {
        AppendHeader("D-Pad");
        AppendButtons({{"Up", DUp}, {"Down", DDown}, {"Left", DLeft}, {"Right", DRight}});
        AppendHeader("Left stick");
        AppendStickDirections(LeftStick);
        AppendButtons({{"Pressed", LStick}});
        AppendStickExtras(LeftStick);
    }
    else if (gamecube)
    {
        AppendHeader("Control stick");
        AppendStickDirections(LeftStick);
        AppendStickExtras(LeftStick);
    }

    if (full || right)
    {
        AppendHeader("Right stick");
        AppendStickDirections(RightStick);
        AppendButtons({{"Pressed", RStick}});
        AppendStickExtras(RightStick);
    }
    else if (gamecube)
    {
        AppendHeader("C-Stick");
        AppendStickDirections(RightStick);
        AppendStickExtras(RightStick);
    }

    if (style == StylePro || style == StyleHandheld)
    {
        AppendHeader("Triggers");
        AppendButtons({{"L", L}, {"R", R}, {"ZL", ZL}, {"ZR", ZR}});
    }
    else if (style == StyleDualJoycons)
    {
        AppendHeader("Triggers");
        AppendButtons({{"L", L}, {"R", R}, {"ZL", ZL}, {"ZR", ZR}, {"Left SL", SLLeft}, {"Left SR", SRLeft}, {"Right SL", SLRight}, {"Right SR", SRRight}});
    }
    else if (left)
    {
        AppendHeader("Triggers");
        AppendButtons({{"L", L}, {"ZL", ZL}, {"Left SL", SLLeft}, {"Left SR", SRLeft}});
    }
    else if (right)
    {
        AppendHeader("Triggers");
        AppendButtons({{"R", R}, {"ZR", ZR}, {"Right SL", SLRight}, {"Right SR", SRRight}});
    }
    else if (gamecube)
    {
        AppendHeader("Triggers");
        AppendButtons({{"Z", R}, {"L", ZL}, {"R", ZR}});
    }
    CommitBody();
}

void UiActivity::OpenControllerChoices(Choices choices)
{
    m_choices = std::move(choices);
    if (Top() == "controllerChoices")
    {
        RenderCurrentSettings();
    }
    else
    {
        Navigate("controllerChoices");
    }
}

void UiActivity::RenderControllerChoices()
{
    BeginBody();
    if (m_choices.items.empty())
    {
        AppendHeader(m_choices.emptyText);
    }
    for (size_t i = 0; i < m_choices.items.size(); i++)
    {
        AppendLink(m_choices.items[i], "", [this, i] {
            const std::function<void(size_t)> pick = m_choices.pick;
            if (pick)
            {
                pick(i);
            }
        });
    }
    CommitBody();
}
