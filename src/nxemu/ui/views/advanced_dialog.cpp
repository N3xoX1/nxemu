#include "advanced_dialog.h"
#include "hotkeys.h"
#include "rml_events.h"
#include "settings_controls.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cstdlib>
#include <nxemu/ui/i18n/translation.h>
#include <string_view>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-os/os_types.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/settings/ui_identifiers.h>
#include <nxemu/settings/ui_settings.h>

using namespace SettingsControls;

namespace
{
constexpr const char * kTabs[] = {"graphics", "system", "application", "shortcuts"};
constexpr const char * kHotkeyPrefix = "hotkey:";
constexpr const char * kTogglePrefix = "toggle:";
constexpr const char * kTabPrefix = "tab:";
constexpr const char * kDefaultAudioDevice = "auto";
constexpr int kSpeedMin = 25; // of the limit, in percent of the console's speed
constexpr int kSpeedMax = 300;

// Where a switch keeps its value.
enum class Store
{
    Module,     // a setting of a module
    Core,       // a setting of NXEmu's core, saved apart
    Interface,  // a setting of the interface, saved with the others of the interface
};

// A switch of the page.
struct Toggle
{
    const char * id;
    const char * setting;
    Store store;
    bool live; // a running game takes the change; otherwise it reads the setting when it starts
};

constexpr Toggle kToggles[] = {
    {"gpu-sync", NXVideoSetting::GpuCommandSynchronization, Store::Module, true},
    {"fast-readback", NXVideoSetting::FastGpuReadback, Store::Module, true},
    {"reactive-flushing", NXVideoSetting::EnableReactiveFlushing, Store::Module, false},
    {"feedback-loops", NXVideoSetting::BarrierFeedbackLoops, Store::Module, false},
    {"fast-gpu-time", NXVideoSetting::FastGPUTime, Store::Module, true},
    {"video-framerate", NXVideoSetting::SyncToFramerateOfVideoPlayback, Store::Module, false},
    {"disk-cache", NXVideoSetting::UseDiskPipelineCache, Store::Module, false},
    {"record-recipes", NXVideoSetting::RecordShaderRecipes, Store::Module, false},
    {"driver-cache", NXVideoSetting::UseVulkanPipelineCache, Store::Module, false},
    {"async-shaders", NXVideoSetting::UseAsynchronousShaderBuilding, Store::Module, false},
    {"async-gpu", NXVideoSetting::UseAsynchronousGPUEmulation, Store::Module, false},
    {"async-present", NXVideoSetting::EnableAsynchronousPresentation, Store::Module, false},
    {"max-clocks", NXVideoSetting::ForceMaximumClocks, Store::Module, false},
    {"limit", NXOsSetting::UseSpeedLimit, Store::Module, true},
    {"multicore", NXOsSetting::UseMultiCore, Store::Module, false},
    {"cpu-hle", NXOsSetting::CpuHleSynchronization, Store::Module, false},
    {"vibration", NXOsSetting::VibrationEnabled, Store::Module, true},
    {"accurate-vibration", NXOsSetting::EnableAccurateVibrations, Store::Module, true},
    {"start-fullscreen", NXUISetting::StartGamesInFullscreen, Store::Interface, true},
    {"start-ui-hidden", NXUISetting::StartGamesWithUiHidden, Store::Interface, true},
    {"confirm-stop", NXUISetting::ConfirmBeforeStopping, Store::Interface, true},
    {"discord", NXUISetting::EnableDiscordPresence, Store::Interface, true},
    {"cpu-backend-info", NXUISetting::ShowCpuBackend, Store::Interface, true},
    {"log-console", NXCoreSetting::ShowLogConsole, Store::Core, true},
};

// A list of the page whose setting is a number.
struct List
{
    const char * id;
    const char * setting;
    bool live;
    Choices (*choices)();
};

Choices AntiAliasings()
{
    return {
        Numbered(Tr("None"), static_cast<int>(AntiAliasing::None), Tr("The picture as the game draws it.")),
        Numbered("FXAA", static_cast<int>(AntiAliasing::Fxaa), Tr("Light work, a little blur. The steadier picture at low resolutions.")),
        Numbered("SMAA", static_cast<int>(AntiAliasing::Smaa), Tr("The best picture, for more work.")),
    };
}

Choices Anisotropies()
{
    return {
        Numbered(Tr("Automatic"), static_cast<int>(AnisotropyMode::Automatic), Tr("Follows the resolution chosen.")),
        Numbered(Tr("As the game asks"), static_cast<int>(AnisotropyMode::Default), Tr("What the console would do.")),
        Numbered("2x", static_cast<int>(AnisotropyMode::X2), {}),
        Numbered("4x", static_cast<int>(AnisotropyMode::X4), {}),
        Numbered("8x", static_cast<int>(AnisotropyMode::X8), {}),
        Numbered("16x", static_cast<int>(AnisotropyMode::X16), Tr("The sharpest. Light work for most graphics devices.")),
    };
}

Choices Accuracies()
{
    return {
        Numbered(Tr("Normal"), static_cast<int>(GpuAccuracy::Normal), Tr("The fastest. Most games look right; particles often do not.")),
        Numbered(Tr("High"), static_cast<int>(GpuAccuracy::High), Tr("What some games need to look right. Some need it from their start.")),
        Numbered(Tr("Extreme"), static_cast<int>(GpuAccuracy::Extreme), Tr("Slow: to look for a fault.")),
    };
}

// In the order the video module numbers them.
Choices MemoryReads()
{
    return {
        Numbered(Tr("As the accuracy level"), 0, Tr("Safe when the accuracy level is High or Extreme.")),
        Numbered(Tr("Unsafe"), 1, Tr("Faster.")),
        Numbered(Tr("Safe"), 2, Tr("Steadier: it fixes some games.")),
    };
}

Choices AstcDecodings()
{
    return {
        Numbered(Tr("Processor"), static_cast<int>(AstcDecodeMode::Cpu), Tr("The slowest, and the safest.")),
        Numbered(Tr("Graphics device"), static_cast<int>(AstcDecodeMode::Gpu), Tr("The usual choice.")),
        Numbered(Tr("Processor, in the background"), static_cast<int>(AstcDecodeMode::CpuAsynchronous), Tr("No stutter, but textures show wrong until they are decoded.")),
    };
}

Choices AstcRecompressions()
{
    return {
        Numbered(Tr("None"), static_cast<int>(AstcRecompression::Uncompressed), Tr("The best picture, and the most memory.")),
        Numbered(Tr("BC1 (low quality)"), static_cast<int>(AstcRecompression::Bc1), Tr("The least memory.")),
        Numbered(Tr("BC3 (medium quality)"), static_cast<int>(AstcRecompression::Bc3), Tr("Less memory.")),
    };
}

Choices VramUsages()
{
    return {
        Numbered(Tr("Conservative"), static_cast<int>(VramUsageMode::Conservative), Tr("Leaves memory to the other applications.")),
        Numbered(Tr("Aggressive"), static_cast<int>(VramUsageMode::Aggressive), Tr("Takes what there is: other applications can slow down. No effect on graphics built into the processor.")),
    };
}

Choices VideoDecodings()
{
    return {
        Numbered(Tr("Graphics device"), static_cast<int>(NvdecEmulation::Gpu), Tr("The fastest in most cases.")),
        Numbered(Tr("Processor"), static_cast<int>(NvdecEmulation::Cpu), Tr("When the graphics device shows videos wrong.")),
        Numbered(Tr("None"), static_cast<int>(NvdecEmulation::Off), Tr("Videos stay black.")),
    };
}

Choices ProcessorEngines()
{
    Choices choices = {
        Numbered(Tr("Translated (Dynarmic)"), static_cast<int>(CpuBackend::Dynarmic), Tr("The code is translated for this computer's processor. Runs every game.")),
    };
    if (SettingsStore::GetInstance().GetDefaultInt(NXCpuSetting::CpuBackend) == static_cast<int>(CpuBackend::Nce))
    {
        choices.insert(choices.begin(), Numbered(Tr("Native (NCE)"), static_cast<int>(CpuBackend::Nce), Tr("The code runs as it is on this computer's processor. The fastest; the games it cannot run are translated.")));
    }
    return choices;
}

Choices ProcessorAccuracies()
{
    return {
        Numbered(Tr("Automatic"), static_cast<int>(CpuAccuracy::Auto), Tr("The usual choice.")),
        Numbered(Tr("Accurate"), static_cast<int>(CpuAccuracy::Accurate), Tr("Slower: for a game that misbehaves.")),
        Numbered(Tr("Unsafe"), static_cast<int>(CpuAccuracy::Unsafe), Tr("Faster, and some games break.")),
        Numbered(Tr("Paranoid"), static_cast<int>(CpuAccuracy::Paranoid), Tr("Very slow: to look for a fault.")),
    };
}

Choices Memories()
{
    return {
        Numbered(Tr("4 GB"), static_cast<int>(MemoryLayout::Memory_4Gb), Tr("What the console has.")),
        Numbered(Tr("6 GB"), static_cast<int>(MemoryLayout::Memory_6Gb), Tr("For mods that need more console memory.")),
        Numbered(Tr("8 GB"), static_cast<int>(MemoryLayout::Memory_8Gb), Tr("For mods that need more than the console has. Nothing gained otherwise.")),
    };
}

Choices NXShaderModes()
{
    return {
        Numbered(Tr("Full warmup (default)"),0,{}),
        Numbered(Tr("Progressive (experimental)"),1,{}),
    };
}
Choices NXShaderMinimums()
{
    Choices choices;
    for (const int value : {0,64,128,256,512,1024,2048,4096,8192})
        choices.push_back(Numbered(std::to_string(value),value,{}));
    return choices;
}
Choices NXShaderDurations()
{
    Choices choices;
    for (const int value : {0,5,15,30,60,90,120,180,300,600})
        choices.push_back(Numbered(std::to_string(value),value,{}));
    return choices;
}

constexpr List kLists[] = {
    {"nxshader-mode", NXVideoSetting::NXShaderWarmupMode, false, NXShaderModes},
    {"nxshader-minimum", NXVideoSetting::NXShaderInitialMinimum, false, NXShaderMinimums},
    {"nxshader-seconds", NXVideoSetting::NXShaderInitialSeconds, false, NXShaderDurations},
    {"antialiasing", NXVideoSetting::AntiAliasing, true, AntiAliasings},
    {"anisotropy", NXVideoSetting::AnisotropicFiltering, false, Anisotropies},
    {"accuracy", NXVideoSetting::AccuracyLevel, true, Accuracies},
    {"dma", NXVideoSetting::DMAAccuracy, true, MemoryReads},
    {"astc", NXVideoSetting::AstcDecodeMode, false, AstcDecodings},
    {"astc-recompression", NXVideoSetting::ASTCRecompressionMethod, false, AstcRecompressions},
    {"vram", NXVideoSetting::VRAMUsageMode, false, VramUsages},
    {"nvdec", NXVideoSetting::NvdecEmulation, false, VideoDecodings},
    {"cpu-backend", NXCpuSetting::CpuBackend, false, ProcessorEngines},
    {"cpu-accuracy", NXCpuSetting::CpuAccuracy, false, ProcessorAccuracies},
    {"memory", NXOsSetting::MemoryLayout, false, Memories},
};

// In the order the audio engines are numbered.
constexpr const char * kAudioEngines[] = {TrMark("Automatic"), "Cubeb", "SDL", TrMark("None (no sound)"), "Oboe"};

bool Get(const Toggle & toggle, const SettingsContext & settings)
{
    return settings.GetBool(toggle.setting);
}

// A switch that does nothing with the current value of another setting: shown off and locked.
bool Inapplicable(const Toggle & toggle, const SettingsContext & settings)
{
    const std::string_view id = toggle.id;
    if (id == "cpu-hle")
    {
        return !settings.GetBool(NXOsSetting::UseMultiCore);
    }
    if (id == "fast-readback")
    {
        // Normal accuracy already lets games read the results without waiting for the GPU.
        return settings.GetInt(NXVideoSetting::AccuracyLevel) == static_cast<int>(GpuAccuracy::Normal);
    }
    return false;
}

void Set(const Toggle & toggle, bool value, SettingsContext & settings)
{
    switch (toggle.store)
    {
    case Store::Interface:
        settings.SetBool(toggle.setting, value);
        SaveUISetting();
        break;
    case Store::Core:
        settings.SetBool(toggle.setting, value);
        SaveCoreSetting();
        break;
    case Store::Module: settings.SetBool(toggle.setting, value); break;
    }
}

void CollectDevice(const char * device, void * userData)
{
    static_cast<std::vector<std::string> *>(userData)->emplace_back(device);
}
} // namespace

AdvancedDialog::AdvancedDialog(UiWindow & window, EmulatorSession & session, SettingsContext & settings, const std::filesystem::path & assetDir, OpenSettingsPage openPage) :
    Dialog(window, assetDir / "advanced_dialog.rml", kSettingsNarrowBelow, kSettingsWidth, kSettingsHeight),
    m_session(session),
    m_settings(settings),
    m_openPage(std::move(openPage))
{
}

void AdvancedDialog::ShowPage(SettingsPage page)
{
    m_debug = page == SettingsPage::Debug;
    if (IsVisible())
    {
        m_hotkeyAsked = -1;
        m_hotkeyNote.clear();
        Document()->SetClass("confirm-defaults", false);
        ShowTab();
        ShowHotkeys();
    }
    else
    {
        Show();
    }
}

void AdvancedDialog::RefreshLanguage()
{
    if (!IsVisible())
    {
        return;
    }
    RefreshTranslation();
    FillLists();
    ShowValues();
    m_changes.clear();
}

void AdvancedDialog::OnOpen()
{
    m_running = m_session.IsRunning() && !m_settings.IsGame();
    m_logFilterLeft = false;
    if (m_settings.IsGame() && (m_tab == "application" || m_tab == "shortcuts")) m_tab = "graphics";
    m_hotkeyAsked = -1;
    m_hotkeyNote.clear();
    FillLists();
    ShowTab();
    ShowValues();
    // Filling the controls is not the user changing them.
    m_changes.clear();
    UiEvents::Listen(Document(), "change", [this](Rml::Event & event) {
        const std::string id = event.GetTargetElement()->GetId();
        // Typing only edits the filter: it is taken when the field is left
        // or validated.
        if (id != "log-filter")
        {
            m_changes.push_back(id);
        }
        else if (event.GetParameter<bool>("linebreak", false))
        {
            m_logFilterLeft = true;
        }
    });
    UiEvents::Listen(Document()->GetElementById("log-filter"), "blur", [this](Rml::Event &) { m_logFilterLeft = true; });
    // A press anywhere but on the shortcut that waits for its key gives it up.
    UiEvents::Listen(Document(), "mousedown", [this](Rml::Event & event) {
        Rml::Element * button = UiEvents::ClosestWithClass(event.GetTargetElement(), "hotkey");
        if (m_hotkeyAsked >= 0 && (button == nullptr || !button->IsClassSet("asking")))
        {
            m_hotkeyAsked = -1;
            m_hotkeyNote.clear();
            ShowHotkeys();
        }
    });
}

void AdvancedDialog::OnUpdate()
{
    std::vector<std::string> changes;
    changes.swap(m_changes);
    for (const std::string & id : changes)
    {
        ApplyChange(id);
    }
    if (m_logFilterLeft)
    {
        m_logFilterLeft = false;
        ApplyLogFilter();
    }
    if (m_running != (m_session.IsRunning() && !m_settings.IsGame()))
    {
        m_running = !m_running;
        ShowValues();
    }
}

void AdvancedDialog::OnCommand(const std::string & command)
{
    if (m_settings.InheritCommand(command))
    {
        FillLists();
        m_changes.clear();
        ShowValues();
        return;
    }
    SettingsPage page;
    if (SettingsPageOf(command, page))
    {
        if (page == SettingsPage::Advanced || page == SettingsPage::Debug)
        {
            ApplyLogFilter();
            ShowPage(page);
            return;
        }
        if (m_openPage(page))
        {
            OnCloseRequested();
        }
    }
    else if (command.rfind(kTabPrefix, 0) == 0)
    {
        if (m_settings.IsGame() && (command == "tab:application" || command == "tab:shortcuts")) return;
        m_tab = command.substr(std::string(kTabPrefix).size());
        ShowTab();
    }
    else if (command.rfind("clear-keyboard:", 0) == 0 || command.rfind("clear-gamepad:", 0) == 0)
    {
        const bool gamepad = command.rfind("clear-gamepad:", 0) == 0;
        const int index = std::atoi(command.c_str() + (gamepad ? 14 : 15));
        if (index < 0 || index >= static_cast<int>(std::size(Hotkeys::kAll)))
        {
            return;
        }
        const HotkeyId id = Hotkeys::kAll[index].id;
        if (gamepad)
        {
            Hotkeys::SetGamepad(id, {});
        }
        else
        {
            Hotkeys::Set(id, {});
        }
        m_hotkeyAsked = -1;
        m_hotkeyNote.clear();
        ShowHotkeys();
    }
    else if (command.rfind(kHotkeyPrefix, 0) == 0 || command.rfind("gamepad:", 0) == 0)
    {
        // A second click on the shortcut that waits gives it up.
        const bool gamepad = command.rfind("gamepad:", 0) == 0;
        const int asked = std::atoi(command.c_str() + (gamepad ? 8 : std::string(kHotkeyPrefix).size()));
        m_hotkeyAsked = (asked == m_hotkeyAsked && gamepad == m_hotkeyGamepad) || asked < 0 || asked >= static_cast<int>(std::size(Hotkeys::kAll)) ? -1 : asked;
        m_hotkeyGamepad = gamepad;
        m_hotkeyDevice = -1;
        if (gamepad && m_hotkeyAsked >= 0 && !Hotkeys::kAll[m_hotkeyAsked].inGame)
        {
            m_hotkeyAsked = -1;
        }
        m_hotkeyNote.clear();
        ShowHotkeys();
    }
    else if (command.rfind(kTogglePrefix, 0) == 0)
    {
        const std::string id = command.substr(std::string(kTogglePrefix).size());
        const auto toggle = std::find_if(std::begin(kToggles), std::end(kToggles), [&id](const Toggle & candidate) { return id == candidate.id; });
        if (toggle != std::end(kToggles) && (!m_settings.IsGame() || toggle->store == Store::Module) && (toggle->live || !m_running) &&
            !Inapplicable(*toggle, m_settings))
        {
            Set(*toggle, !Get(*toggle, m_settings), m_settings);
            m_unsaved = m_unsaved || toggle->store == Store::Module;
            ShowValues();
        }
    }
    else if (command == "defaults" || command == "defaults-keep")
    {
        // Asked first: there is no going back.
        Document()->SetClass("confirm-defaults", command == "defaults");
    }
    else if (command == "defaults-confirmed")
    {
        Document()->SetClass("confirm-defaults", false);
        RestoreDefaults();
    }
}

void AdvancedDialog::OnCloseRequested()
{
    ApplyLogFilter();
    if (m_unsaved)
    {
        m_unsaved = false;
        if (!m_settings.IsGame()) m_session.FlushSettings();
    }
    Close();
}

void AdvancedDialog::FillLists()
{
    SettingsContext & settings = m_settings;
    for (const List & list : kLists)
    {
        const Choices choices = list.choices();
        // A value that is no longer offered gives way to the usual one.
        const std::string value = std::to_string(settings.GetInt(list.setting));
        if (!m_settings.IsGame() && std::none_of(choices.begin(), choices.end(), [&value](const Choice & choice) { return choice.value == value; }))
        {
            settings.SetInt(list.setting, settings.GetDefaultInt(list.setting));
            m_unsaved = true;
        }
        Fill(Select(Document(), list.id), choices, std::to_string(settings.GetInt(list.setting)));
    }

    // The engines this computer has, and the microphones of the one chosen.
    const int engine = settings.GetInt(NXOsSetting::AudioSinkId);
    std::vector<uint32_t> engines(std::size(kAudioEngines));
    uint32_t engineCount = 0;
    std::vector<std::string> inputs;
    if (m_session.IsValid())
    {
        IOperatingSystem & os = m_session.Modules().OperatingSystem();
        os.AudioGetSyncIDs(engines.data(), static_cast<uint32_t>(engines.size()), &engineCount);
        os.AudioGetDeviceListForSink(static_cast<uint32_t>(engine), true, CollectDevice, &inputs);
    }
    engines.resize(std::min<size_t>(engineCount, engines.size()));
    Choices choices;
    choices.push_back(Numbered(Tr(kAudioEngines[0]), 0, Tr("The one that suits this computer.")));
    for (const uint32_t id : engines)
    {
        if (id > 0 && id < std::size(kAudioEngines))
        {
            choices.push_back(Numbered(Tr(kAudioEngines[id]), static_cast<int>(id), {}));
        }
    }
    Fill(Select(Document(), "audio-engine"), choices, std::to_string(engine));

    const std::string input = settings.GetString(NXOsSetting::AudioInputDeviceId);
    choices.clear();
    choices.push_back({Tr("System default"), kDefaultAudioDevice, Tr("The one the computer records from.")});
    bool listed = input == kDefaultAudioDevice || input.empty();
    for (const std::string & device : inputs)
    {
        if (device != kDefaultAudioDevice)
        {
            choices.push_back({device, device, {}});
            listed = listed || device == input;
        }
    }
    if (!listed)
    {
        choices.push_back({Tr("%1 (not connected)", {input}), input, Tr("Games listen to the default of the computer until it is back.")});
    }
    Fill(Select(Document(), "audio-input"), choices, input.empty() ? kDefaultAudioDevice : input);

    const std::string output = settings.GetString(NXOsSetting::AudioOutputDeviceId);
    std::vector<std::string> outputs;
    if (m_session.IsValid())
    {
        m_session.Modules().OperatingSystem().AudioGetDeviceListForSink(static_cast<uint32_t>(engine), false, CollectDevice, &outputs);
    }
    choices.clear();
    choices.push_back({Tr("System default"), kDefaultAudioDevice, Tr("The one the computer plays its sounds on.")});
    listed = output == kDefaultAudioDevice || output.empty();
    for (const std::string & device : outputs)
    {
        if (device != kDefaultAudioDevice)
        {
            choices.push_back({device, device, {}});
            listed = listed || device == output;
        }
    }
    if (!listed)
    {
        choices.push_back({Tr("%1 (not connected)", {output}), output, Tr("Games play on the default of the computer until it is back.")});
    }
    Fill(Select(Document(), "audio-device"), choices, output.empty() ? kDefaultAudioDevice : output);

    const int audioMode = settings.GetInt(NXOsSetting::AudioMode);
    choices.clear();
    choices.push_back(Numbered(Tr("Stereo"), static_cast<int>(AudioMode::Stereo), Tr("Two channels: headphones and most speakers.")));
    choices.push_back(Numbered(Tr("Surround"), static_cast<int>(AudioMode::Surround), Tr("Six channels, for speakers set around the room.")));
    if (audioMode == static_cast<int>(AudioMode::Mono))
    {
        choices.push_back(Numbered(Tr("Mono"), audioMode, Tr("One channel.")));
    }
    Fill(Select(Document(), "audio-mode"), choices, std::to_string(audioMode));
}

bool AdvancedDialog::HandleKey(const SDL_KeyboardEvent & event)
{
    if (!IsVisible() || m_hotkeyAsked < 0)
    {
        return false;
    }
    const Shortcut shortcut = Shortcut::Of(event.keysym);
    // A modifier alone is the start of a shortcut.
    if (event.type != SDL_KEYDOWN || event.repeat != 0 || shortcut.key == SDLK_UNKNOWN)
    {
        return true;
    }
    const Hotkeys::Info & asked = Hotkeys::kAll[m_hotkeyAsked];
    if (!shortcut.ctrl && !shortcut.alt && !shortcut.shift && (shortcut.key == SDLK_DELETE || shortcut.key == SDLK_BACKSPACE))
    {
        if (m_hotkeyGamepad)
        {
            Hotkeys::SetGamepad(asked.id, {});
        }
        else
        {
            Hotkeys::Set(asked.id, {});
        }
        m_hotkeyAsked = -1;
        m_hotkeyNote.clear();
        ShowHotkeys();
        return true;
    }
    if (m_hotkeyGamepad)
    {
        if (shortcut.key == SDLK_ESCAPE)
        {
            m_hotkeyAsked = -1;
        }
        ShowHotkeys();
        return true;
    }
    if (!Hotkeys::Usable(shortcut))
    {
        m_hotkeyNote = Tr("A shortcut is a function key, or a key with %1 or Alt.", {CommandKeyboard() ? "Cmd" : "Ctrl"});
    }
    else if (const Hotkeys::Info * holder = Hotkeys::Holder(shortcut, asked.id))
    {
        m_hotkeyNote = Tr("%1 is already the shortcut of \"%2\".", {shortcut.Label(), Tr(holder->label)});
    }
    else
    {
        Hotkeys::Set(asked.id, shortcut);
        m_hotkeyAsked = -1;
        m_hotkeyNote.clear();
    }
    ShowHotkeys();
    return true;
}

// The shortcuts, each on a button that asks for its key.
void AdvancedDialog::ShowHotkeys()
{
    const auto binding = [this](int index, bool gamepad) {
        const HotkeyId id = Hotkeys::kAll[index].id;
        const bool asking = index == m_hotkeyAsked && gamepad == m_hotkeyGamepad;
        const bool assigned = gamepad ? Hotkeys::GetGamepad(id).buttons != 0 : Hotkeys::Get(id).key != SDLK_UNKNOWN;
        const std::string label = asking      ? (gamepad ? Tr("Press the buttons...") : Tr("Press the keys..."))
                                  : !assigned ? Tr("Not assigned")
                                  : gamepad   ? Hotkeys::GetGamepad(id).Label()
                                              : Hotkeys::Get(id).Label();
        return "<div class=\"hotkey-binding\"><button class=\"row-button hotkey" + std::string(asking ? " asking" : "") + "\" data-command=\"" +
               (gamepad ? "gamepad:" : kHotkeyPrefix) + std::to_string(index) + "\">" + Rml::StringUtilities::EncodeRml(label) +
               "</button><button class=\"hotkey-clear\" data-command=\"clear-" + (gamepad ? "gamepad:" : "keyboard:") + std::to_string(index) + "\" title=\"" +
               Rml::StringUtilities::EncodeRml(Tr("Clear shortcut")) + "\"" + (assigned ? "" : " disabled") + ">&#xD7;</button></div>";
    };
    std::string application;
    std::string game = "<div class=\"row hotkey-columns\"><div class=\"row-texts\"/><div class=\"hotkey\">" + Rml::StringUtilities::EncodeRml(Tr("Keyboard")) +
                       "</div><div class=\"hotkey\">" + Rml::StringUtilities::EncodeRml(Tr("Gamepad")) + "</div></div>";
    for (int index = 0; index < static_cast<int>(std::size(Hotkeys::kAll)); ++index)
    {
        const Hotkeys::Info & info = Hotkeys::kAll[index];
        (info.inGame ? game : application) += "<div class=\"row\"><div class=\"row-texts\"><div class=\"row-label\">" +
                                              Rml::StringUtilities::EncodeRml(Tr(info.label)) + "</div></div>" + binding(index, false) +
                                              (info.inGame ? binding(index, true) : "") + "</div>";
    }
    const auto show = [this](const char * id, const std::string & rml) {
        Rml::Element * element = Document()->GetElementById(id);
        if (element != nullptr && element->GetInnerRML() != rml)
        {
            element->SetInnerRML(rml);
        }
    };
    show("hotkeys-application", application);
    show("hotkeys-game", game);
    SetText("hotkey-note", Rml::StringUtilities::EncodeRml(m_hotkeyNote));
}

bool AdvancedDialog::HandleGamepad(const SDL_Event & event, const Hotkeys::GamepadShortcut & chord)
{
    if (!IsVisible() || m_hotkeyAsked < 0 || !m_hotkeyGamepad)
    {
        return false;
    }
    if (event.type == SDL_JOYDEVICEREMOVED && event.jdevice.which == m_hotkeyDevice)
    {
        m_hotkeyDevice = -1;
    }
    if (event.type != SDL_JOYBUTTONDOWN && event.type != SDL_JOYBUTTONUP)
    {
        return false;
    }
    if (event.type == SDL_JOYBUTTONDOWN && m_hotkeyDevice < 0)
    {
        m_hotkeyDevice = event.jbutton.which;
    }
    if (event.type != SDL_JOYBUTTONUP || event.jbutton.which != m_hotkeyDevice || chord.buttons == 0)
    {
        return true;
    }
    const HotkeyId id = Hotkeys::kAll[m_hotkeyAsked].id;
    if (const Hotkeys::Info * holder = Hotkeys::GamepadHolder(chord, id))
    {
        m_hotkeyNote = Tr("%1 is already the shortcut of \"%2\".", {chord.Label(), Tr(holder->label)});
        m_hotkeyDevice = -1;
    }
    else
    {
        Hotkeys::SetGamepad(id, chord);
        m_hotkeyAsked = -1;
        m_hotkeyNote.clear();
    }
    ShowHotkeys();
    return true;
}

void AdvancedDialog::ShowTab()
{
    for (const char * tab : kTabs)
    {
        Document()->SetClass(std::string("tab-") + tab, !m_debug && m_tab == tab);
        SetClass(Document(), (std::string("tab-button-") + tab).c_str(), "selected", m_tab == tab);
    }
    Document()->SetClass("page-debug", m_debug);
    SetClass(Document(), "category-advanced", "selected", !m_debug);
    SetClass(Document(), "category-debug", "selected", m_debug);
}

void AdvancedDialog::ShowValues()
{
    SettingsContext & settings = m_settings;
    Document()->SetClass("game-running", m_running);
    for (const Toggle & toggle : kToggles)
    {
        const bool inapplicable = Inapplicable(toggle, m_settings);
        SetClass(Document(), toggle.id, "on", Get(toggle, m_settings) && !inapplicable);
        SetClass(Document(), toggle.id, "locked", (m_running && !toggle.live) || inapplicable);
    }
    for (const List & list : kLists)
    {
        Choose(Select(Document(), list.id), std::to_string(settings.GetInt(list.setting)));
        SetDisabled(Document(), list.id, m_running && !list.live);
    }
    const int speed = std::clamp(settings.GetInt(NXOsSetting::SpeedLimit), kSpeedMin, kSpeedMax);
    SetClass(Document(), "speed", "off", !settings.GetBool(NXOsSetting::UseSpeedLimit));
    SetRange(Document(), "speed-range", speed);
    SetText("speed-text", std::to_string(speed) + "%");
    SetDisabled(Document(), "audio-engine", m_running);
    SetDisabled(Document(), "audio-device", m_running);
    SetDisabled(Document(), "audio-input", m_running);

    ShowHotkeys();
    m_settings.Decorate(Document());
    auto * filter = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("log-filter"));
    if (filter != nullptr && filter->GetValue() != settings.GetString(NXCoreSetting::LogFilter))
    {
        filter->SetValue(settings.GetString(NXCoreSetting::LogFilter));
    }
}

void AdvancedDialog::ApplyChange(const std::string & id)
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
    if (m_settings.IsGame() && (id == "audio-device" || id == "audio-input" || id == "audio-engine" || id == "log-filter")) return;
    const int value = std::atoi(text.c_str());

    const char * setting = nullptr;
    if (id == "audio-mode")
    {
        if (value != settings.GetInt(NXOsSetting::AudioMode))
        {
            settings.SetInt(NXOsSetting::AudioMode, value);
            m_unsaved = true;
        }
        return;
    }
    if (id == "audio-device")
    {
        if (!m_running && text != settings.GetString(NXOsSetting::AudioOutputDeviceId))
        {
            settings.SetString(NXOsSetting::AudioOutputDeviceId, text.c_str());
            m_unsaved = true;
        }
        return;
    }
    if (id == "audio-input")
    {
        if (!m_running && text != settings.GetString(NXOsSetting::AudioInputDeviceId))
        {
            settings.SetString(NXOsSetting::AudioInputDeviceId, text.c_str());
            m_unsaved = true;
        }
        return;
    }
    if (id == "audio-engine")
    {
        if (!m_running && value != settings.GetInt(NXOsSetting::AudioSinkId))
        {
            settings.SetInt(NXOsSetting::AudioSinkId, value);
            m_unsaved = true;
            // The microphones are those of the engine.
            FillLists();
            m_changes.clear();
        }
        return;
    }
    if (id == "speed-range")
    {
        // The slider shows a limit out of its reach at its end: that is not
        // the user choosing the end.
        if (value == std::clamp(settings.GetInt(NXOsSetting::SpeedLimit), kSpeedMin, kSpeedMax))
        {
            return;
        }
        setting = NXOsSetting::SpeedLimit;
    }
    else
    {
        const auto list = std::find_if(std::begin(kLists), std::end(kLists), [&id](const List & candidate) { return id == candidate.id; });
        if (list == std::end(kLists) || (m_running && !list->live))
        {
            return;
        }
        setting = list->setting;
    }
    if (settings.GetInt(setting) != value)
    {
        settings.SetInt(setting, value);
        m_unsaved = true;
        ShowValues();
    }
}

void AdvancedDialog::ApplyLogFilter()
{
    if (m_settings.IsGame()) return;
    SettingsContext & settings = m_settings;
    auto * filter = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("log-filter"));
    if (filter == nullptr)
    {
        return;
    }
    // An empty filter writes nothing of use: the usual one takes its place.
    const std::string value = filter->GetValue().empty() ? settings.GetDefaultString(NXCoreSetting::LogFilter) : filter->GetValue();
    if (value != settings.GetString(NXCoreSetting::LogFilter))
    {
        settings.SetString(NXCoreSetting::LogFilter, value.c_str());
        SaveCoreSetting();
    }
    ShowValues();
}

void AdvancedDialog::RestoreDefaults()
{
    if (m_settings.IsGame())
    {
        m_settings.InheritAll();
        FillLists();
        m_changes.clear();
        ShowValues();
        return;
    }
    SettingsContext & settings = m_settings;
    // What a running game holds stays as it is.
    for (const Toggle & toggle : kToggles)
    {
        if (toggle.live || !m_running)
        {
            Set(toggle, settings.GetDefaultBool(toggle.setting), m_settings);
        }
    }
    for (const List & list : kLists)
    {
        if (list.live || !m_running)
        {
            settings.SetInt(list.setting, settings.GetDefaultInt(list.setting));
        }
    }
    settings.SetInt(NXOsSetting::SpeedLimit, settings.GetDefaultInt(NXOsSetting::SpeedLimit));
    settings.SetInt(NXOsSetting::AudioMode, settings.GetDefaultInt(NXOsSetting::AudioMode));
    settings.SetString(NXCoreSetting::LogFilter, settings.GetDefaultString(NXCoreSetting::LogFilter));
    Hotkeys::RestoreDefaults();
    m_hotkeyAsked = -1;
    m_hotkeyNote.clear();
    SaveCoreSetting();
    if (!m_running)
    {
        settings.SetInt(NXOsSetting::AudioSinkId, settings.GetDefaultInt(NXOsSetting::AudioSinkId));
        settings.SetString(NXOsSetting::AudioInputDeviceId, settings.GetDefaultString(NXOsSetting::AudioInputDeviceId));
        settings.SetString(NXOsSetting::AudioOutputDeviceId, settings.GetDefaultString(NXOsSetting::AudioOutputDeviceId));
    }
    m_unsaved = true;
    FillLists();
    m_changes.clear();
    ShowValues();
}
