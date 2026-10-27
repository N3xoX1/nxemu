#include "controller_dialog.h"
#include "controller_picture.h"
#include "rml_events.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/sdl_input.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/input/input_text.h>

namespace
{
constexpr float kStackedBelow = 900.0f;
constexpr uint64_t kListenMs = 5000;
constexpr uint64_t kSequenceGapMs = 450;
constexpr uint64_t kDevicesEveryMs = 1000;
constexpr int kMaxInputsPerFrame = 64;
constexpr float kStickPressed = 0.5f;
constexpr float kStickTravel = 46.0f;
constexpr float kPictureWidth = 430.0f;
constexpr float kPictureHeight = 325.0f;
constexpr float kPictureMargin = 8.0f;

enum class SlotKind
{
    Button,
    StickDirection,
    StickModifier,
    Motion,
};

enum TypeBit : uint32_t
{
    kPro = 1 << 0,
    kDual = 1 << 1,
    kJoyLeft = 1 << 2,
    kJoyRight = 1 << 3,
    kHandheld = 1 << 4,
    kGameCube = 1 << 5,

    kLeftSide = kPro | kDual | kJoyLeft | kHandheld,
    kRightSide = kPro | kDual | kJoyRight | kHandheld,
};

struct Slot
{
    const char * group;
    SlotKind kind;
    uint32_t index;
    const char * direction;
    const char * label;
    uint32_t types;
};

constexpr uint32_t B(NativeButtonValues button)
{
    return static_cast<uint32_t>(button);
}

constexpr Slot kSlots[] = {
    {"lstick", SlotKind::StickDirection, 0, "up", TrMark("Up"), kLeftSide | kGameCube},
    {"lstick", SlotKind::StickDirection, 0, "left", TrMark("Left"), kLeftSide | kGameCube},
    {"lstick", SlotKind::StickDirection, 0, "right", TrMark("Right"), kLeftSide | kGameCube},
    {"lstick", SlotKind::StickDirection, 0, "down", TrMark("Down"), kLeftSide | kGameCube},
    {"lstick", SlotKind::Button, B(NativeButtonValues::LStick), nullptr, TrMark("Pressed"), kLeftSide},
    {"lstick", SlotKind::StickModifier, 0, nullptr, TrMark("Modifier"), kLeftSide | kGameCube},
    {"dpad", SlotKind::Button, B(NativeButtonValues::DUp), nullptr, TrMark("Up"), kLeftSide | kGameCube},
    {"dpad", SlotKind::Button, B(NativeButtonValues::DLeft), nullptr, TrMark("Left"), kLeftSide | kGameCube},
    {"dpad", SlotKind::Button, B(NativeButtonValues::DRight), nullptr, TrMark("Right"), kLeftSide | kGameCube},
    {"dpad", SlotKind::Button, B(NativeButtonValues::DDown), nullptr, TrMark("Down"), kLeftSide | kGameCube},
    {"face", SlotKind::Button, B(NativeButtonValues::X), nullptr, "X", kRightSide | kGameCube},
    {"face", SlotKind::Button, B(NativeButtonValues::Y), nullptr, "Y", kRightSide | kGameCube},
    {"face", SlotKind::Button, B(NativeButtonValues::A), nullptr, "A", kRightSide | kGameCube},
    {"face", SlotKind::Button, B(NativeButtonValues::B), nullptr, "B", kRightSide | kGameCube},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::L), nullptr, "L", kLeftSide},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::ZL), nullptr, "ZL", kLeftSide | kGameCube},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::ZR), nullptr, "ZR", kRightSide | kGameCube},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::R), nullptr, "R", kRightSide | kGameCube},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::SLLeft), nullptr, TrMark("Left SL"), kDual | kJoyLeft},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::SRLeft), nullptr, TrMark("Left SR"), kDual | kJoyLeft},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::SLRight), nullptr, TrMark("Right SL"), kDual | kJoyRight},
    {"shoulder", SlotKind::Button, B(NativeButtonValues::SRRight), nullptr, TrMark("Right SR"), kDual | kJoyRight},
    {"system", SlotKind::Button, B(NativeButtonValues::Minus), nullptr, TrMark("Minus"), kLeftSide},
    {"system", SlotKind::Button, B(NativeButtonValues::Screenshot), nullptr, TrMark("Capture"), kLeftSide},
    {"system", SlotKind::Button, B(NativeButtonValues::Home), nullptr, TrMark("Home"), kRightSide},
    {"system", SlotKind::Button, B(NativeButtonValues::Plus), nullptr, TrMark("Plus"), kRightSide | kGameCube},
    {"rstick", SlotKind::StickDirection, 1, "up", TrMark("Up"), kRightSide | kGameCube},
    {"rstick", SlotKind::StickDirection, 1, "left", TrMark("Left"), kRightSide | kGameCube},
    {"rstick", SlotKind::StickDirection, 1, "right", TrMark("Right"), kRightSide | kGameCube},
    {"rstick", SlotKind::StickDirection, 1, "down", TrMark("Down"), kRightSide | kGameCube},
    {"rstick", SlotKind::Button, B(NativeButtonValues::RStick), nullptr, TrMark("Pressed"), kRightSide},
    {"rstick", SlotKind::StickModifier, 1, nullptr, TrMark("Modifier"), kRightSide | kGameCube},
    {"motion", SlotKind::Motion, 0, nullptr, TrMark("Motion 1"), kLeftSide},
    {"motion", SlotKind::Motion, 1, nullptr, TrMark("Motion 2"), kDual | kJoyRight},
};
constexpr int kSlotCount = static_cast<int>(sizeof(kSlots) / sizeof(kSlots[0]));
constexpr const char * kGroups[] = {"lstick", "dpad", "face", "shoulder", "system", "rstick", "motion"};
constexpr uint32_t kButtonCount = static_cast<uint32_t>(NativeButtonValues::NumButtons);
constexpr uint32_t kStickCount = static_cast<uint32_t>(NativeAnalogValues::NumAnalogs);
constexpr uint32_t kMotionCount = static_cast<uint32_t>(NativeMotionValues::NumMotions);

constexpr NpadIdType kPlayerIds[] = {NpadIdType::Player1, NpadIdType::Player2, NpadIdType::Player3, NpadIdType::Player4,
                                     NpadIdType::Player5, NpadIdType::Player6, NpadIdType::Player7, NpadIdType::Player8};

struct TypeChoice
{
    NpadStyleIndex type;
    NpadStyleSet style;
    uint32_t bit;
    const char * name;
};

constexpr TypeChoice kTypes[] = {
    {NpadStyleIndex::Fullkey, NpadStyleSet::Fullkey, kPro, TrMark("Pro Controller")},
    {NpadStyleIndex::JoyconDual, NpadStyleSet::JoyDual, kDual, TrMark("Dual Joy-Cons")},
    {NpadStyleIndex::JoyconLeft, NpadStyleSet::JoyLeft, kJoyLeft, TrMark("Left Joy-Con")},
    {NpadStyleIndex::JoyconRight, NpadStyleSet::JoyRight, kJoyRight, TrMark("Right Joy-Con")},
    {NpadStyleIndex::Handheld, NpadStyleSet::Handheld, kHandheld, TrMark("Handheld")},
    {NpadStyleIndex::GameCube, NpadStyleSet::Gc, kGameCube, TrMark("GameCube Controller")},
};

// Names in nxemu-os/os_enum_strings.cpp. Settings must preserve the existing
// serialized strings; never persist numeric NpadStyleIndex values.
constexpr const char * kStoredTypes[] = {
    "ProController", "DualJoyconDetached", "LeftJoycon", "RightJoycon", "Handheld", "GameCube"};

const char * SavedType(NpadStyleIndex type)
{
    for (size_t i = 0; i < std::size(kTypes); ++i)
    {
        if (kTypes[i].type == type) { return kStoredTypes[i]; }
    }
    return kStoredTypes[0];
}

NpadStyleIndex LoadedType(const JsonValue & value)
{
    if (value.isString())
    {
        for (size_t i = 0; i < std::size(kTypes); ++i)
        {
            if (value.asString() == kStoredTypes[i]) { return kTypes[i].type; }
        }
    }
    return kTypes[0].type;
}

const TypeChoice & ChoiceOf(NpadStyleIndex type)
{
    for (const TypeChoice & choice : kTypes)
    {
        if (choice.type == type)
        {
            return choice;
        }
    }
    return kTypes[0];
}

constexpr int kKeyButtons[kButtonCount] = {
    'C', 'X', 'V', 'Z', 'F', 'G', 'Q', 'E', 'R', 'T', 'M', 'N', 0x25, 0x26, 0x27, 0x28, 'Q', 'E', 0, 0, 'Q', 'E',
};
constexpr const char * kDirections[] = {"up", "down", "left", "right"};
constexpr int kKeySticks[kStickCount][4] = {{'W', 'S', 'A', 'D'}, {'I', 'K', 'J', 'L'}};
constexpr int kKeyStickModifiers[kStickCount] = {0x10, 0};
constexpr int kKeyMotions[kMotionCount] = {'7', '8'};

struct StickOption
{
    const char * name;
    const char * param;
    float fallback;
};
constexpr StickOption kStickOptions[] = {{"deadzone", "deadzone", 0.15f}, {"range", "range", 0.95f}, {"modifier", "modifier_scale", 0.5f}};

const char * VibrationEnabledSetting(int player)
{
    static const char * const ids[] = {
        NXOsSetting::Player0VibrationEnabled, NXOsSetting::Player1VibrationEnabled, NXOsSetting::Player2VibrationEnabled,
        NXOsSetting::Player3VibrationEnabled, NXOsSetting::Player4VibrationEnabled, NXOsSetting::Player5VibrationEnabled,
        NXOsSetting::Player6VibrationEnabled, NXOsSetting::Player7VibrationEnabled,
    };
    return ids[player];
}

const char * VibrationStrengthSetting(int player)
{
    static const char * const ids[] = {
        NXOsSetting::Player0VibrationStrength, NXOsSetting::Player1VibrationStrength, NXOsSetting::Player2VibrationStrength,
        NXOsSetting::Player3VibrationStrength, NXOsSetting::Player4VibrationStrength, NXOsSetting::Player5VibrationStrength,
        NXOsSetting::Player6VibrationStrength, NXOsSetting::Player7VibrationStrength,
    };
    return ids[player];
}

int Percent(float value)
{
    return static_cast<int>(std::lround(value * 100.0f));
}

std::string Escaped(const std::string & text)
{
    std::string result;
    for (const char c : text)
    {
        switch (c)
        {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        default: result += c; break;
        }
    }
    return result;
}

bool SameDevice(const IParamPackage & a, const IParamPackage & b)
{
    return std::string(a.GetString("engine", "")) == b.GetString("engine", "") && std::string(a.GetString("guid", "")) == b.GetString("guid", "") &&
           a.GetInt("port", 0) == b.GetInt("port", 0) && a.GetInt("pad", 0) == b.GetInt("pad", 0);
}

bool Alone(const Slot & slot)
{
    if (slot.kind == SlotKind::StickDirection)
    {
        return std::string(slot.direction) == "up" || std::string(slot.direction) == "down";
    }
    return slot.kind == SlotKind::Button && (slot.index == B(NativeButtonValues::DUp) || slot.index == B(NativeButtonValues::DDown) ||
                                             slot.index == B(NativeButtonValues::X) || slot.index == B(NativeButtonValues::B));
}

int SlotOfButton(uint32_t button)
{
    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        if (kSlots[slot].kind == SlotKind::Button && kSlots[slot].index == button)
        {
            return slot;
        }
    }
    return -1;
}

std::string Dp(float value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fdp", value);
    return text;
}

void SetDp(Rml::Element * element, Rml::PropertyId property, float value)
{
    element->SetProperty(property, Rml::Property(value, Rml::Unit::DP));
}

void SetShown(Rml::Element * element, bool shown)
{
    if (element != nullptr)
    {
        element->SetClass("hidden", !shown);
    }
}

void SetRange(Rml::Element * element, int value)
{
    auto * input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(element);
    if (input != nullptr && std::atoi(input->GetValue().c_str()) != value)
    {
        input->SetValue(std::to_string(value));
    }
}

int RangeValue(Rml::Element * element, int fallback)
{
    auto * input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(element);
    return input != nullptr ? std::atoi(input->GetValue().c_str()) : fallback;
}
} // namespace

ControllerDialog::ControllerDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir) :
    Dialog(window.Context(), assetDir / "controller_dialog.rml", kSettingsNarrowBelow),
    m_window(window),
    m_session(session)
{
}

ControllerDialog::~ControllerDialog()
{
    Abandon();
}

void ControllerDialog::HandleKey(const SDL_KeyboardEvent & event)
{
    if (!IsVisible()) { return; }
    if (m_offline)
    {
        OfflineKey(event);
    }
    else if (m_session.Inputs().Available() && event.keysym.sym != SDLK_ESCAPE)
    {
        ForwardKeyboardEvent(event, Os());
    }
}

void ControllerDialog::Abandon()
{
    if (IsVisible())
    {
        StopListening();
        m_rows.clear();
        Close();
    }
}

IOperatingSystem & ControllerDialog::Os() const
{
    return m_session.Modules().OperatingSystem();
}

bool ControllerDialog::Connected(int player) const
{
    return m_offline ? m_offlineConnected[player] : Pad(player).IsConnected();
}

IEmulatedController & ControllerDialog::Pad(int player) const
{
    return Os().GetEmulatedController(player == 0 && m_handheld ? NpadIdType::Handheld : kPlayerIds[player]);
}

NpadStyleIndex ControllerDialog::Type(int player) const
{
    return m_offline ? m_offlineTypes[player] :
           player == 0 && m_handheld ? NpadStyleIndex::Handheld : Pad(player).GetNpadStyleIndex();
}

void ControllerDialog::SetType(int player, NpadStyleIndex type)
{
    if (m_offline)
    {
        m_offlineTypes[player] = type;
        m_offlineTypeChanged[player] = true;
        if (player == 0) { m_handheld = type == NpadStyleIndex::Handheld; }
        m_offlineChanged[player] = true;
        m_changed = true;
        return;
    }
    IEmulatedController & from = Pad(player);
    const bool connected = from.IsConnected();
    from.Disconnect();
    if (player == 0)
    {
        m_handheld = type == NpadStyleIndex::Handheld;
        IEmulatedController & to = Pad(player);
        if (&to != &from)
        {
            for (uint32_t i = 0; i < kButtonCount; ++i)
            {
                to.SetButtonParam(i, InputParams::Take(from.GetButtonParamPtr(i)));
            }
            for (uint32_t i = 0; i < kStickCount; ++i)
            {
                to.SetStickParam(i, InputParams::Take(from.GetStickParamPtr(i)));
            }
            for (uint32_t i = 0; i < kMotionCount; ++i)
            {
                to.SetMotionParam(i, InputParams::Take(from.GetMotionParamPtr(i)));
            }
        }
        Os().GetEmulatedController(NpadIdType::Player1).SetNpadStyleIndex(type);
        if (m_handheld)
        {
            to.SetNpadStyleIndex(type);
        }
    }
    else
    {
        from.SetNpadStyleIndex(type);
    }
    if (connected)
    {
        Pad(player).Connect();
    }
    m_changed = true;
}

void ControllerDialog::SetConnected(int player, bool connected)
{
    if (m_offline)
    {
        m_offlineConnected[player] = connected;
        m_offlineChanged[player] = true;
        m_changed = true;
        return;
    }
    if (connected)
    {
        Pad(player).Connect();
    }
    else
    {
        Pad(player).Disconnect();
    }
    m_changed = true;
}

void ControllerDialog::Finish(bool save)
{
    StopListening();
    if (m_offline)
    {
        if (save)
        {
            for (int player = 0; player < kPlayers; ++player)
            {
                if (m_offlineChanged[player] && !StageOfflineBindings(player))
                {
                    SetText("hint", Tr("Invalid controller settings."));
                    return;
                }
            }
            if (!m_session.Inputs().SaveOfflinePlayers(m_offlineSettings, m_offlineChanged))
            {
                SetText("hint", Tr("Could not save controller settings."));
                return;
            }
        }
        m_rows.clear();
        Close();
        return;
    }
    IOperatingSystem & os = Os();
    std::vector<IEmulatedController *> pads = {&os.GetEmulatedController(NpadIdType::Handheld)};
    for (const NpadIdType id : kPlayerIds)
    {
        pads.push_back(&os.GetEmulatedController(id));
    }
    if (save)
    {
        for (IEmulatedController * pad : pads)
        {
            pad->SaveCurrentConfig();
        }
        for (int player = 0; player < kPlayers; ++player)
        {
            SettingsStore::GetInstance().SetBool(VibrationEnabledSetting(player), m_vibration[player]);
            SettingsStore::GetInstance().SetInt(VibrationStrengthSetting(player), m_strength[player]);
        }
        m_session.FlushSettings();
    }
    else if (m_changed)
    {
        for (IEmulatedController * pad : pads)
        {
            pad->ReloadFromSettings();
        }
    }
    m_rows.clear();
    Close();
}

void ControllerDialog::OnOpen()
{
    m_offline = !m_session.Inputs().Available();
    if (m_offline) { LoadOfflinePlayers(); }
    Document()->SetClass("offline", m_offline);
    IOperatingSystem * os = m_offline ? nullptr : &Os();
    m_player = 0;
    m_changed = false;
    m_listenSlot = -1;
    m_handheld = m_offline ? m_offlineTypes[0] == NpadStyleIndex::Handheld :
                 os->GetEmulatedController(NpadIdType::Handheld).IsConnected() ||
                 os->GetEmulatedController(NpadIdType::Player1).GetNpadStyleIndex() == NpadStyleIndex::Handheld;
    for (int player = 0; player < kPlayers; ++player)
    {
        if (!m_offline)
        {
            m_vibration[player] = SettingsStore::GetInstance().GetBool(VibrationEnabledSetting(player));
            m_strength[player] = SettingsStore::GetInstance().GetInt(VibrationStrengthSetting(player));
        }
    }

    m_marks.clear();
    m_pictureShown = false;
    Document()->SetClass("stacked", WindowWidth() < kStackedBelow);
    BuildRows();
    ReadDevices();
    if (m_offline) { ShowPhysicalDevices(m_session.Inputs().RefreshPhysicalDevices()); }
    for (int player = 0; player < kPlayers; ++player)
    {
        m_device[player] = MappedDevice(player);
    }
    FillDevices();

    m_changes.clear();
    UiEvents::Listen(Document(), "change", [this](Rml::Event & event) { m_changes.push_back(event.GetTargetElement()->GetId()); });
    UiEvents::ListenFirst(Document(), "keydown", [](Rml::Event & event) {
        if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) != Rml::Input::KI_ESCAPE)
        {
            event.StopPropagation();
        }
    });
    ShowPlayers();
    ShowPlayer();
}

void ControllerDialog::OnUpdate()
{
    std::vector<std::string> changes;
    changes.swap(m_changes);
    for (const std::string & id : changes)
    {
        ApplyChange(id);
    }
    if (!m_offline && SDL_GetTicks64() - m_devicesRead >= kDevicesEveryMs && !Listening())
    {
        const std::vector<InputParams> before = std::move(m_devices);
        ReadDevices();
        const bool same = before.size() == m_devices.size() &&
                          std::equal(before.begin(), before.end(), m_devices.begin(), [](const InputParams & a, const InputParams & b) { return a.Text() == b.Text(); });
        if (!same)
        {
            for (int player = 0; player < kPlayers; ++player)
            {
                m_device[player] = MappedDevice(player);
            }
            FillDevices();
        }
    }
    Document()->SetClass("stacked", WindowWidth() < kStackedBelow);
    ShowPicture();
    PollListening();
    ShowState();
    ShowHint();
}

void ControllerDialog::OnCommand(const std::string & command)
{
    const size_t colon = command.find(':');
    const std::string name = command.substr(0, colon);
    const int number = colon != std::string::npos ? std::atoi(command.c_str() + colon + 1) : 0;
    if (name == "refresh-physical" && m_offline)
    {
        ShowPhysicalDevices(m_session.Inputs().RefreshPhysicalDevices());
        return;
    }
    if (name == "save" || name == "cancel")
    {
        Finish(name == "save");
        return;
    }
    const int listened = m_listenSlot;
    StopListening();
    if (name == "player" && number >= 0 && number < kPlayers)
    {
        m_player = number;
        ShowPlayers();
        ShowPlayer();
    }
    else if (name == "bind" && number >= 0 && number < kSlotCount && number != listened)
    {
        Listen(number, false);
    }
    else if (name == "clear" && number >= 0 && number < kSlotCount)
    {
        Bind(number, InputParams());
    }
    else if (name == "toggle-connected")
    {
        SetConnected(m_player, !Connected(m_player));
        ShowPlayers();
        ShowPlayer();
    }
    else if (name == "toggle-vibration")
    {
        m_vibration[m_player] = !m_vibration[m_player];
        if (m_offline) { m_offlineChanged[m_player] = true; }
        m_changed = true;
        ShowPlayer();
    }
    else if (name == "map-all")
    {
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            if (SlotShown(slot) && kSlots[slot].kind != SlotKind::Motion && kSlots[slot].kind != SlotKind::StickModifier)
            {
                Listen(slot, true);
                break;
            }
        }
    }
    else if (name == "defaults")
    {
        ApplyDevice(m_player, m_device[m_player]);
        m_vibration[m_player] = true;
        m_strength[m_player] = 100;
        if (m_offline) { m_offlineChanged[m_player] = true; }
        m_changed = true;
        ShowPlayer();
    }
}

void ControllerDialog::OnCloseRequested()
{
    if (Listening())
    {
        StopListening();
    }
    else
    {
        Finish(false);
    }
}

void ControllerDialog::BuildRows()
{
    for (const char * group : kGroups)
    {
        std::string rml;
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            if (std::string(kSlots[slot].group) == group)
            {
                const std::string number = std::to_string(slot);
                rml += "<div id=\"bind-" + number + "\" class=\"bind" + (Alone(kSlots[slot]) ? " alone" : "") + "\"" +
                       (kSlots[slot].kind == SlotKind::Motion ? " title=\"" + Tr("The controller's gyroscope, for games played by tilting or shaking it") + "\"" : std::string()) + " data-command=\"bind:" + number + "\"><div class=\"bind-label\" />"
                       "<div class=\"bind-value\" /><div class=\"bind-clear\" data-command=\"clear:" + number + "\" title=\"" + Tr("Unbind") + "\">&#xD7;</div></div>";
            }
        }
        if (Rml::Element * rows = Document()->GetElementById(std::string("rows-") + group))
        {
            rows->SetInnerRML(rml);
        }
    }
    m_rows.assign(kSlotCount, nullptr);
    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        m_rows[slot] = Document()->GetElementById("bind-" + std::to_string(slot));
    }
}

void ControllerDialog::ReadDevices()
{
    m_devices.clear();
    m_devicesRead = SDL_GetTicks64();
    if (m_offline)
    {
        InputParams any;
        any.Set("engine", "any");
        m_devices.push_back(std::move(any));
        InputParams keyboard;
        keyboard.Set("engine", "keyboard");
        m_devices.push_back(std::move(keyboard));
        return;
    }
    // This snapshot is owned by the frontend; it survives a guest-module reset.
    HostInputService & inputs = m_session.Inputs();
    inputs.RefreshDevices();
    m_devices = inputs.Devices();
}

void ControllerDialog::ShowPhysicalDevices(bool available)
{
    if (!m_offline) { return; }
    if (!available)
    {
        SetText("physical-device-names", Tr("Controller detection unavailable"));
        return;
    }
    const auto & devices = m_session.Inputs().PhysicalDevices();
    if (devices.empty())
    {
        SetText("physical-device-names", Tr("No physical gamepad detected"));
        return;
    }
    std::string names = Tr("Detected (read-only): ");
    for (size_t i = 0; i < devices.size() && i < 8; ++i)
    {
        if (i > 0) { names += ", "; }
        names += Escaped(devices[i].name);
    }
    if (devices.size() > 8) { names += " (+" + std::to_string(devices.size() - 8) + ")"; }
    // SetText takes RML, so external hardware names must be escaped.
    SetText("physical-device-names", names);
}

void ControllerDialog::FillDevices()
{
    auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("device"));
    if (select == nullptr)
    {
        return;
    }
    select->RemoveAll();
    for (size_t i = 0; i < m_devices.size(); ++i)
    {
        const std::string engine = m_devices[i].Get("engine");
        const std::string name = engine == "any" ? Tr("Any device") : engine == "keyboard" ? Tr("Keyboard") : m_devices[i].GetString("display", Tr("Unknown").c_str());
        select->Add(Escaped(name), std::to_string(i));
    }
    select->SetSelection(m_device[m_player]);
}

void ControllerDialog::FillTypes()
{
    auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("type"));
    if (select == nullptr)
    {
        return;
    }
    const uint32_t supported = m_offline ? ~0u : static_cast<uint32_t>(Os().GetSupportedStyleTag());
    const NpadStyleIndex current = Type(m_player);
    select->RemoveAll();
    int index = 0;
    for (const TypeChoice & choice : kTypes)
    {
        // Handheld has a second persisted controller (player_8). Until its
        // independent configuration is supported offline, do not offer a
        // new handheld conversion that would drop those bindings.
        const bool offered = (supported & static_cast<uint32_t>(choice.style)) != 0 &&
                             (choice.type != NpadStyleIndex::Handheld || (m_player == 0 && !m_offline));
        if (offered || choice.type == current)
        {
            select->Add(Tr(choice.name), std::to_string(static_cast<int>(choice.type)));
            if (choice.type == current)
            {
                select->SetSelection(index);
            }
            ++index;
        }
    }
}

int ControllerDialog::MappedDevice(int player) const
{
    const NpadIdType id = player == 0 && m_handheld ? NpadIdType::Handheld : kPlayerIds[player];
    if (m_offline)
    {
        for (const InputParams & binding : m_offlineMappings[player].buttons)
        {
            if (binding.Get("engine") == "keyboard") { return 1; }
        }
        return 0;
    }
    const std::vector<InputParams> mapped = m_session.Inputs().MappedDevices(id);
    int found = 0;
    const size_t count = mapped.size();
    if (count == 1)
    {
        for (size_t i = 1; i < m_devices.size(); ++i)
        {
            if (SameDevice(m_devices[i], mapped[0]))
            {
                found = static_cast<int>(i);
                break;
            }
        }
    }
    else if (count == 2)
    {
        const InputParams & first = mapped[0];
        const InputParams & second = mapped[1];
        const std::string guid1 = first.Get("guid"), guid2 = second.Get("guid");
        for (size_t i = 1; i < m_devices.size(); ++i)
        {
            const InputParams & device = m_devices[i];
            const bool pair = (device.Get("guid") == guid1 && device.Get("guid2") == guid2) || (device.Get("guid") == guid2 && device.Get("guid2") == guid1);
            if (pair && device.Get("engine") == first.Get("engine") && device.Get("engine") == second.Get("engine") &&
                device.GetInt("port", 0) == first.GetInt("port", 0) && first.GetInt("port", 0) == second.GetInt("port", 0))
            {
                found = static_cast<int>(i);
                break;
            }
        }
    }
    return found;
}

void ControllerDialog::ApplyDevice(int player, int device)
{
    if (device < 0 || device >= static_cast<int>(m_devices.size()))
    {
        return;
    }
    const InputParams & source = m_devices[device];
    const std::string engine = source.Get("engine");
    std::array<InputParams, kButtonCount> buttons;
    std::array<InputParams, kStickCount> sticks;
    std::array<InputParams, kMotionCount> motions;
    if (engine == "any" || engine == "keyboard")
    {
        for (uint32_t i = 0; i < kButtonCount; ++i)
        {
            buttons[i] = InputParams::Key(kKeyButtons[i]);
        }
        for (uint32_t stick = 0; stick < kStickCount; ++stick)
        {
            sticks[stick].Set("engine", "analog_from_button");
            for (int direction = 0; direction < 4; ++direction)
            {
                sticks[stick].Set(kDirections[direction], InputParams::Key(kKeySticks[stick][direction]).Text());
            }
            sticks[stick].Set("modifier", InputParams::Key(kKeyStickModifiers[stick]).Text());
        }
        for (uint32_t i = 0; i < kMotionCount; ++i)
        {
            motions[i] = InputParams::Key(kKeyMotions[i]);
        }
    }
    else
    {
        // The host service copies and releases every OS-owned mapping list.
        // Nothing is applied until all three lists were read successfully.
        auto mappings = m_session.Inputs().ReadDeviceMappings(source);
        if (!mappings) { return; }
        buttons = std::move(mappings->buttons);
        sticks = std::move(mappings->sticks);
        motions = std::move(mappings->motions);
    }
    if (m_offline)
    {
        auto & mappings = m_offlineMappings[player];
        mappings.buttons = std::move(buttons);
        mappings.sticks = std::move(sticks);
        mappings.motions = std::move(motions);
        m_offlineChanged[player] = true;
        m_changed = true;
        return;
    }
    IEmulatedController & pad = Pad(player);
    for (uint32_t i = 0; i < kButtonCount; ++i)
    {
        pad.SetButtonParam(i, buttons[i]);
    }
    for (uint32_t i = 0; i < kStickCount; ++i)
    {
        pad.SetStickParam(i, sticks[i]);
    }
    for (uint32_t i = 0; i < kMotionCount; ++i)
    {
        pad.SetMotionParam(i, motions[i]);
    }
    m_changed = true;
}

void ControllerDialog::ShowPlayers()
{
    std::string rml;
    for (int player = 0; player < kPlayers; ++player)
    {
        const bool connected = Connected(player);
        rml += std::string("<div class=\"player-tab") + (player == m_player ? " selected" : "") + (connected ? " connected" : "") + "\" data-command=\"player:" +
               std::to_string(player) + "\" title=\"" + (connected ? Tr(ChoiceOf(Type(player)).name) : Tr("Not connected")) +
               "\"><div class=\"player-dot\" /><div>" + Tr("Player %1", {std::to_string(player + 1)}) + "</div></div>";
    }
    SetText("players", rml);
}

void ControllerDialog::ShowPlayer()
{
    if (Rml::Element * connected = Document()->GetElementById("connected"))
    {
        connected->SetClass("on", Connected(m_player));
    }
    FillTypes();
    if (auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(Document()->GetElementById("device")))
    {
        if (select->GetSelection() != m_device[m_player])
        {
            select->SetSelection(m_device[m_player]);
        }
    }
    if (Rml::Element * vibration = Document()->GetElementById("vibration"))
    {
        vibration->SetClass("on", m_vibration[m_player]);
    }
    SetRange(Document()->GetElementById("strength-range"), m_strength[m_player]);
    SetText("strength-text", std::to_string(m_strength[m_player]) + "%");
    SetShown(Document()->GetElementById("strength"), m_vibration[m_player]);
    ShowBindings();
}

void ControllerDialog::ShowBindings()
{
    const bool gameCube = Type(m_player) == NpadStyleIndex::GameCube;
    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        Rml::Element * row = m_rows[slot];
        if (row == nullptr)
        {
            continue;
        }
        const Slot & info = kSlots[slot];
        std::string label = Tr(info.label);
        if (gameCube && info.kind == SlotKind::Button)
        {
            label = info.index == B(NativeButtonValues::Plus) ? Tr("Start") : info.index == B(NativeButtonValues::ZL) ? "L" :
                    info.index == B(NativeButtonValues::ZR) ? "R" : info.index == B(NativeButtonValues::R) ? "Z" : label;
        }
        const std::string text = SlotText(slot);
        const bool listening = slot == m_listenSlot;
        row->SetClass("hidden", !SlotShown(slot));
        row->SetClass("unset", text.empty() && !listening);
        row->SetClass("listening", listening);
        if (Rml::Element * element = row->GetChild(0); element->GetInnerRML() != label)
        {
            element->SetInnerRML(label);
        }
        const std::string value = listening ? Tr("Press...") : text.empty() ? Tr("Not set") : Escaped(text);
        if (Rml::Element * element = row->GetChild(1); element->GetInnerRML() != value)
        {
            element->SetInnerRML(value);
        }
    }
    for (const char * group : kGroups)
    {
        bool any = false;
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            any = any || (std::string(kSlots[slot].group) == group && SlotShown(slot));
        }
        SetShown(Document()->GetElementById(std::string("group-") + group), any);
    }
    SetText("title-lstick", gameCube ? Tr("Control stick") : Tr("Left stick"));
    SetText("title-rstick", gameCube ? Tr("C-stick") : Tr("Right stick"));
    ShowPicture();
    ShowSticks();
}

void ControllerDialog::ShowSticks()
{
    for (uint32_t stick = 0; stick < kStickCount; ++stick)
    {
        const InputParams params = m_offline ? m_offlineMappings[m_player].sticks[stick] :
                                   InputParams::Take(Pad(m_player).GetStickParamPtr(stick));
        const bool analog = !params.Unset() && (m_offline ? params.Has("axis_x") : Os().IsController(params));
        const bool keys = params.Get("engine") == "analog_from_button";
        const std::string prefix = "stick-" + std::to_string(stick) + "-";
        for (const StickOption & option : kStickOptions)
        {
            const bool shown = std::string(option.name) == "modifier" ? keys : analog;
            const int percent = Percent(params.GetFloat(option.param, option.fallback));
            SetShown(Document()->GetElementById(prefix + option.name), shown);
            SetRange(Document()->GetElementById(prefix + option.name + "-range"), percent);
            SetText((prefix + option.name + "-text").c_str(), std::to_string(percent) + "%");
        }
        if (Rml::Element * zone = Document()->GetElementById(prefix + "zone"))
        {
            const float size = analog ? params.GetFloat("deadzone", 0.15f) * m_stickView[stick] * kStickTravel / 50.0f : 0.0f;
            SetDp(zone, Rml::PropertyId::Width, size);
            SetDp(zone, Rml::PropertyId::Height, size);
            SetDp(zone, Rml::PropertyId::Left, (m_stickView[stick] - size) / 2.0f);
            SetDp(zone, Rml::PropertyId::Top, (m_stickView[stick] - size) / 2.0f);
            for (const Rml::PropertyId corner : {Rml::PropertyId::BorderTopLeftRadius, Rml::PropertyId::BorderTopRightRadius,
                                                 Rml::PropertyId::BorderBottomLeftRadius, Rml::PropertyId::BorderBottomRightRadius})
            {
                SetDp(zone, corner, size / 2.0f);
            }
            SetShown(zone, size >= 2.0f);
        }
    }
}

void ControllerDialog::ShowPicture()
{
    Rml::Element * host = Document()->GetElementById("picture");
    const NpadStyleIndex type = Type(m_player);
    Rml::Element * column = Document()->GetElementById("centre");
    const float columnWidth = column != nullptr ? column->GetClientWidth() / Document()->GetContext()->GetDensityIndependentPixelRatio() : 0.0f;
    if (columnWidth <= kPictureMargin)
    {
        return;
    }
    const float room = std::floor(std::min(kPictureWidth, columnWidth - kPictureMargin));
    if (host == nullptr || (m_pictureShown && m_pictureType == type && m_pictureRoom == room))
    {
        return;
    }
    m_pictureShown = true;
    m_pictureType = type;
    m_pictureRoom = room;
    m_marks.clear();
    m_stickView.fill(0.0f);

    const ControllerPicture & picture = PictureOf(type);
    const float scale = std::min(room / picture.width, room * kPictureHeight / kPictureWidth / picture.height);
    const float width = picture.width * scale;
    const float height = picture.height * scale;
    std::string rml = std::string("<img src=\"") + picture.image + "\" style=\"width: " + Dp(width) + "; height: " + Dp(height) + ";\" />";
    m_capTravel.fill(0.0f);
    for (size_t index = 0; index < picture.partCount; ++index)
    {
        const ControllerPart & part = picture.parts[index];
        if (part.kind == ControllerPart::Kind::StickCap && picture.stickCaps[part.index] != nullptr)
        {
            m_capTravel[part.index] = picture.stickTravel[part.index] * width;
            rml += "<img id=\"stick-" + std::to_string(part.index) + "-cap\" class=\"stick-cap\" src=\"" + picture.stickCaps[part.index] + "\" style=\"left: " +
                   Dp(part.x * width) + "; top: " + Dp(part.y * height) + "; width: " + Dp(part.width * width) + "; height: " + Dp(part.height * height) + ";\" />";
        }
    }
    for (size_t index = 0; index < picture.partCount; ++index)
    {
        const ControllerPart & part = picture.parts[index];
        const float w = part.width * width;
        const float h = part.height * height;
        const std::string box = "left: " + Dp(part.x * width) + "; top: " + Dp(part.y * height) + "; width: " + Dp(w) + "; height: " + Dp(h) + ";";
        if (part.kind == ControllerPart::Kind::StickCap)
        {
            continue;
        }
        if (part.kind == ControllerPart::Kind::StickView)
        {
            const std::string prefix = "stick-" + std::to_string(part.index) + "-";
            m_stickView[part.index] = std::min(w, h);
            rml += "<div class=\"stick-view\" style=\"" + box + "\"><div id=\"" + prefix + "zone\" class=\"stick-zone\" /><div id=\"" + prefix +
                   "dot\" class=\"stick-dot\" /></div>";
            continue;
        }
        const uint32_t button = part.kind == ControllerPart::Kind::Stick ? B(part.index == 0 ? NativeButtonValues::LStick : NativeButtonValues::RStick) : part.index;
        const int slot = SlotOfButton(button);
        if (slot < 0 || !SlotShown(slot))
        {
            continue;
        }
        const bool round = std::abs(w - h) < 0.25f * std::max(w, h);
        rml += "<div id=\"mark-" + std::to_string(m_marks.size()) + "\" class=\"mark\" data-command=\"bind:" + std::to_string(slot) + "\" title=\"" + Tr(kSlots[slot].label) +
               "\" style=\"" + box + " border-radius: " + Dp(round ? std::min(w, h) / 2.0f : 4.0f) + ";\" />";
        m_marks.push_back({nullptr, slot});
    }
    SetDp(host, Rml::PropertyId::Width, width);
    SetDp(host, Rml::PropertyId::Height, height);
    host->SetInnerRML(rml);
    for (size_t index = 0; index < m_marks.size(); ++index)
    {
        m_marks[index].element = Document()->GetElementById("mark-" + std::to_string(index));
    }
}

void ControllerDialog::ShowState()
{
    if (m_offline) { return; } // No emulated HID state until a game session exists.
    IEmulatedController & pad = Pad(m_player);
    button_status_t buttons[kButtonCount]{};
    pad.GetButtonsStatus(buttons, kButtonCount);
    const SticksValues sticks = pad.GetSticksValues();
    std::array<bool, kSlotCount> down{};
    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        const Slot & info = kSlots[slot];
        bool pressed = false;
        if (info.kind == SlotKind::Button)
        {
            pressed = buttons[info.index].value;
        }
        else if (info.kind == SlotKind::StickDirection)
        {
            const StickStatus & stick = sticks.status[info.index];
            const std::string direction = info.direction;
            pressed = direction == "up" ? stick.y.value > kStickPressed : direction == "down" ? stick.y.value < -kStickPressed :
                      direction == "left" ? stick.x.value < -kStickPressed : stick.x.value > kStickPressed;
        }
        down[slot] = pressed;
        if (m_rows[slot] != nullptr)
        {
            m_rows[slot]->SetClass("pressed", pressed);
        }
    }
    for (const Mark & mark : m_marks)
    {
        if (mark.element != nullptr)
        {
            mark.element->SetClass("pressed", down[mark.slot] && mark.slot != m_listenSlot);
            mark.element->SetClass("listening", mark.slot == m_listenSlot);
        }
    }
    for (uint32_t stick = 0; stick < kStickCount; ++stick)
    {
        if (Rml::Element * dot = Document()->GetElementById("stick-" + std::to_string(stick) + "-dot"))
        {
            const float x = std::clamp(sticks.status[stick].x.value, -1.0f, 1.0f);
            const float y = std::clamp(sticks.status[stick].y.value, -1.0f, 1.0f);
            dot->SetProperty(Rml::PropertyId::Left, Rml::Property(50.0f + x * kStickTravel, Rml::Unit::PERCENT));
            dot->SetProperty(Rml::PropertyId::Top, Rml::Property(50.0f - y * kStickTravel, Rml::Unit::PERCENT));
            dot->SetClass("moved", std::abs(x) > 0.01f || std::abs(y) > 0.01f);
        }
        if (Rml::Element * cap = Document()->GetElementById("stick-" + std::to_string(stick) + "-cap"))
        {
            const float x = std::clamp(sticks.status[stick].x.value, -1.0f, 1.0f) * m_capTravel[stick];
            const float y = -std::clamp(sticks.status[stick].y.value, -1.0f, 1.0f) * m_capTravel[stick];
            cap->SetProperty("transform", "translate(" + Dp(x) + ", " + Dp(y) + ")");
        }
    }
}

void ControllerDialog::ShowHint()
{
    Rml::Element * hint = Document()->GetElementById("hint");
    if (hint == nullptr)
    {
        return;
    }
    hint->SetClass("listening", Listening());
    if (!Listening())
    {
        m_shownSeconds = -1;
        SetText("hint", Tr("Click a control or a button of the picture to bind it."));
        return;
    }
    const uint64_t now = SDL_GetTicks64();
    const int seconds = m_listenBegun && m_listenUntil > now ? static_cast<int>((m_listenUntil - now + 999) / 1000) : static_cast<int>(kListenMs / 1000);
    if (seconds == m_shownSeconds)
    {
        return;
    }
    m_shownSeconds = seconds;
    const Slot & info = kSlots[m_listenSlot];
    const std::string how = info.kind == SlotKind::StickDirection ? Tr("Press a key, or move the stick sideways and then up and down.")
                            : info.kind == SlotKind::Motion       ? Tr("Shake a controller that has a gyroscope, or press a key that will stand for a shake.")
                                                                  : Tr("Press a key or a button.");
    SetText("hint", Tr("%1 Esc cancels. (%2)", {how, std::to_string(seconds)}));
}

void ControllerDialog::ApplyChange(const std::string & id)
{
    Rml::Element * element = Document()->GetElementById(id);
    if (element == nullptr)
    {
        return;
    }
    if (id == "type")
    {
        auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(element);
        const NpadStyleIndex type = static_cast<NpadStyleIndex>(std::atoi(select->GetValue().c_str()));
        if (select->GetSelection() >= 0 && type != Type(m_player))
        {
            StopListening();
            SetType(m_player, type);
            ShowPlayers();
            ShowPlayer();
        }
    }
    else if (id == "device")
    {
        auto * select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(element);
        const int device = select->GetSelection();
        if (device >= 0 && device < static_cast<int>(m_devices.size()) && device != m_device[m_player])
        {
            StopListening();
            m_device[m_player] = device;
            if (device != 0)
            {
                ApplyDevice(m_player, device);
            }
            ShowPlayer();
        }
    }
    else if (id == "strength-range")
    {
        const int strength = RangeValue(element, m_strength[m_player]);
        if (strength != m_strength[m_player])
        {
            m_strength[m_player] = strength;
            if (m_offline) { m_offlineChanged[m_player] = true; }
            m_changed = true;
            SetText("strength-text", std::to_string(strength) + "%");
        }
    }
    else
    {
        for (uint32_t stick = 0; stick < kStickCount; ++stick)
        {
            for (const StickOption & option : kStickOptions)
            {
                if (id != "stick-" + std::to_string(stick) + "-" + option.name + "-range")
                {
                    continue;
                }
                InputParams params = m_offline ? m_offlineMappings[m_player].sticks[stick] :
                                     InputParams::Take(Pad(m_player).GetStickParamPtr(stick));
                const int percent = RangeValue(element, 0);
                if (!params.Unset() && percent != Percent(params.GetFloat(option.param, option.fallback)))
                {
                    params.SetFloat(option.param, static_cast<float>(percent) / 100.0f);
                    if (m_offline)
                    {
                        m_offlineMappings[m_player].sticks[stick] = params;
                        m_offlineChanged[m_player] = true;
                    }
                    else { Pad(m_player).SetStickParam(stick, params); }
                    m_changed = true;
                    ShowSticks();
                }
            }
        }
    }
}

InputParams ControllerDialog::SlotParams(int slot) const
{
    const Slot & info = kSlots[slot];
    if (m_offline)
    {
        const auto & mappings = m_offlineMappings[m_player];
        return info.kind == SlotKind::Button ? mappings.buttons[info.index] :
               info.kind == SlotKind::Motion ? mappings.motions[info.index] : mappings.sticks[info.index];
    }
    switch (info.kind)
    {
    case SlotKind::Button: return InputParams::Take(Pad(m_player).GetButtonParamPtr(info.index));
    case SlotKind::Motion: return InputParams::Take(Pad(m_player).GetMotionParamPtr(info.index));
    default: return InputParams::Take(Pad(m_player).GetStickParamPtr(info.index));
    }
}

std::string ControllerDialog::SlotText(int slot) const
{
    const Slot & info = kSlots[slot];
    const InputParams params = SlotParams(slot);
    switch (info.kind)
    {
    case SlotKind::StickDirection: return InputText::StickDirection(m_session.Inputs(), params, info.direction);
    case SlotKind::StickModifier: return InputText::Binding(m_session.Inputs(), InputParams(params.Get("modifier")));
    default: return InputText::Binding(m_session.Inputs(), params);
    }
}

void ControllerDialog::Bind(int slot, const InputParams & input)
{
    const Slot & info = kSlots[slot];
    IEmulatedController * pad = m_offline ? nullptr : &Pad(m_player);
    if (info.kind == SlotKind::Button)
    {
        if (pad) { pad->SetButtonParam(info.index, input); }
        else { m_offlineMappings[m_player].buttons[info.index] = input; }
    }
    else if (info.kind == SlotKind::Motion)
    {
        if (pad) { pad->SetMotionParam(info.index, input); }
        else { m_offlineMappings[m_player].motions[info.index] = input; }
    }
    else
    {
        InputParams stick = SlotParams(slot);
        const bool fromButtons = stick.Get("engine") == "analog_from_button";
        const char * key = info.kind == SlotKind::StickModifier ? "modifier" : info.direction;
        if (input.Has("axis_x") && input.Has("axis_y") && info.kind == SlotKind::StickDirection)
        {
            stick = input;
        }
        else if (input.Unset())
        {
            if (fromButtons)
            {
                stick.Erase(key);
            }
            else if (info.kind == SlotKind::StickDirection)
            {
                stick = InputParams();
            }
        }
        else
        {
            if (!fromButtons)
            {
                stick = InputParams();
                stick.Set("engine", "analog_from_button");
            }
            stick.Set(key, input.Text());
        }
        if (pad) { pad->SetStickParam(info.index, stick); }
        else { m_offlineMappings[m_player].sticks[info.index] = std::move(stick); }
    }
    m_changed = true;
    if (m_offline) { m_offlineChanged[m_player] = true; }
    ShowBindings();
}

bool ControllerDialog::SlotShown(int slot) const
{
    const Slot & info = kSlots[slot];
    if ((info.types & ChoiceOf(Type(m_player)).bit) == 0)
    {
        return false;
    }
    return info.kind != SlotKind::StickModifier || SlotParams(slot).Get("engine") == "analog_from_button";
}

void ControllerDialog::Listen(int slot, bool sequence)
{
    StopListening();
    m_listenSlot = slot;
    m_listenSequence = sequence;
    m_listenBegun = false;
    m_listenFrom = SDL_GetTicks64() + (m_listenIgnore.empty() ? 0 : kSequenceGapMs);
    if (m_offline) { m_listenUntil = m_listenFrom + kListenMs; }
    m_shownSeconds = -1;
    ShowBindings();
}

void ControllerDialog::StopListening()
{
    if (m_listenSlot < 0)
    {
        return;
    }
    if (m_listenBegun && !m_offline)
    {
        m_session.Inputs().StopMapping();
    }
    m_listenSlot = -1;
    m_listenBegun = false;
    m_listenIgnore.clear();
    if (IsVisible() && !m_rows.empty())
    {
        ShowBindings();
    }
}

void ControllerDialog::PollListening()
{
    if (!Listening())
    {
        return;
    }
    if (m_offline)
    {
        if (SDL_GetTicks64() >= m_listenUntil) { StopListening(); }
        return;
    }
    HostInputService & inputs = m_session.Inputs();
    if (!inputs.Available())
    {
        StopListening();
        return;
    }
    const uint64_t now = SDL_GetTicks64();
    const Slot & info = kSlots[m_listenSlot];
    if (!m_listenBegun)
    {
        if (now < m_listenFrom)
        {
            return;
        }
        if (!inputs.BeginMapping(info.kind == SlotKind::StickDirection ? PollingInputType::Stick : info.kind == SlotKind::Motion ? PollingInputType::Motion : PollingInputType::Button))
        {
            StopListening();
            return;
        }
        m_listenBegun = true;
        m_listenUntil = now + kListenMs;
    }
    for (int i = 0; i < kMaxInputsPerFrame; ++i)
    {
        const InputParams input = inputs.NextInput();
        if (input.Unset())
        {
            break;
        }
        if (!Accepts(input) || input.Text() == m_listenIgnore)
        {
            continue;
        }
        const int slot = m_listenSlot;
        const bool sequence = m_listenSequence;
        inputs.StopMapping();
        m_listenSlot = -1;
        m_listenBegun = false;
        Bind(slot, input);

        const bool wholeStick = info.kind == SlotKind::StickDirection && input.Has("axis_x");
        for (int next = slot + 1; sequence && next < kSlotCount; ++next)
        {
            const Slot & candidate = kSlots[next];
            const bool covered = wholeStick && candidate.kind == SlotKind::StickDirection && candidate.index == info.index;
            if (SlotShown(next) && !covered && candidate.kind != SlotKind::Motion && candidate.kind != SlotKind::StickModifier)
            {
                m_listenIgnore = input.Text();
                Listen(next, true);
                break;
            }
        }
        if (!Listening())
        {
            m_listenIgnore.clear();
        }
        return;
    }
    if (now >= m_listenUntil)
    {
        StopListening();
    }
}

bool ControllerDialog::Accepts(const InputParams & input) const
{
    if (m_device[m_player] < 0 || m_device[m_player] >= static_cast<int>(m_devices.size()))
    {
        return true;
    }
    const InputParams & device = m_devices[m_device[m_player]];
    const std::string engine = device.Get("engine");
    if (engine == "any" || input.Has("motion"))
    {
        return true;
    }
    if (input.Get("engine") != engine)
    {
        return false;
    }
    return engine == "keyboard" ||
           ((input.Get("guid") == device.Get("guid") || input.Get("guid") == device.Get("guid2")) && input.GetInt("port", 0) == device.GetInt("port", 0));
}


void ControllerDialog::LoadOfflinePlayers()
{
    m_offlineChanged.fill(false);
    m_offlineTypeChanged.fill(false);
    for (int player = 0; player < kPlayers; ++player)
    {
        JsonValue stored = m_session.Inputs().StoredPlayerOverrides(player);
        m_offlineSettings[player] = stored.isObject() ? stored : JsonValue{};
        // Read through a const view: an absent or malformed field must not
        // accidentally create/mutate the in-memory stored JSON.
        const JsonValue & view = stored;
        m_offlineConnected[player] = view["Connected"].isBool() ? view["Connected"].asBool() : player == 0;
        m_offlineTypes[player] = LoadedType(view["ControllerType"]);
        const JsonValue & vibration = view["Vibration"];
        m_vibration[player] = vibration["Enabled"].isBool() ? vibration["Enabled"].asBool() : true;
        m_strength[player] = vibration["Strength"].isInt() ?
                             static_cast<int>(std::clamp(vibration["Strength"].asInt64(), int64_t{0}, int64_t{100})) : 100;
        auto & mappings = m_offlineMappings[player];
        mappings = HostDeviceMappings{};
        if (player == 0)
        {
            for (uint32_t i = 0; i < kButtonCount; ++i) { mappings.buttons[i] = InputParams::Key(kKeyButtons[i]); }
            for (uint32_t stick = 0; stick < kStickCount; ++stick)
            {
                mappings.sticks[stick].Set("engine", "analog_from_button");
                for (uint32_t direction = 0; direction < 4; ++direction)
                {
                    mappings.sticks[stick].Set(kDirections[direction], InputParams::Key(kKeySticks[stick][direction]).Text());
                }
                mappings.sticks[stick].Set("modifier", InputParams::Key(kKeyStickModifiers[stick]).Text());
            }
            for (uint32_t i = 0; i < kMotionCount; ++i) { mappings.motions[i] = InputParams::Key(kKeyMotions[i]); }
        }
        for (const auto & group : {HostBindingGroup::Button, HostBindingGroup::Analog, HostBindingGroup::Motion})
        {
            const char * groupName = group == HostBindingGroup::Button ? "Button" :
                                     group == HostBindingGroup::Analog ? "Analog" : "Motion";
            const uint32_t count = group == HostBindingGroup::Button ? kButtonCount :
                                   group == HostBindingGroup::Analog ? kStickCount : kMotionCount;
            for (uint32_t index = 0; index < count; ++index)
            {
                const char * key = HostInputService::BindingKey(group, index);
                const JsonValue & value = view[groupName][key];
                if (!value.isString()) { continue; }
                InputParams params(value.asString());
                if (group == HostBindingGroup::Button) { mappings.buttons[index] = std::move(params); }
                else if (group == HostBindingGroup::Analog) { mappings.sticks[index] = std::move(params); }
                else { mappings.motions[index] = std::move(params); }
            }
        }
    }
}

bool ControllerDialog::StageOfflineBindings(int player)
{
    JsonValue & stored = m_offlineSettings[player];
    // Never replace an unknown/malformed subtree of the user's config.
    for (const char * key : {"Button", "Analog", "Motion", "Vibration"})
    {
        if (stored.isObject() && stored.isMember(key) && !stored[key].isObject()) { return false; }
    }
    stored["Connected"] = m_offlineConnected[player];
    if (m_offlineTypeChanged[player] || !stored["ControllerType"].isString())
    {
        stored["ControllerType"] = SavedType(m_offlineTypes[player]);
    }
    stored["Vibration"]["Enabled"] = m_vibration[player];
    stored["Vibration"]["Strength"] = m_strength[player];
    const auto & mappings = m_offlineMappings[player];
    for (uint32_t index = 0; index < kButtonCount; ++index)
    {
        stored["Button"][HostInputService::BindingKey(HostBindingGroup::Button, index)] = mappings.buttons[index].Text();
    }
    for (uint32_t index = 0; index < kStickCount; ++index)
    {
        stored["Analog"][HostInputService::BindingKey(HostBindingGroup::Analog, index)] = mappings.sticks[index].Text();
    }
    for (uint32_t index = 0; index < kMotionCount; ++index)
    {
        stored["Motion"][HostInputService::BindingKey(HostBindingGroup::Motion, index)] = mappings.motions[index].Text();
    }
    return true;
}

void ControllerDialog::OfflineKey(const SDL_KeyboardEvent & event)
{
    if (!Listening() || event.type != SDL_KEYDOWN || event.repeat) { return; }
    if (event.keysym.sym == SDLK_ESCAPE) { StopListening(); return; }
    const int key = KeyboardVirtualKey(event);
    if (key <= 0 || SDL_GetTicks64() < m_listenFrom) { return; }
    const int slot = m_listenSlot;
    const bool sequence = m_listenSequence;
    Bind(slot, InputParams::Key(key));
    m_listenSlot = -1;
    m_listenBegun = false;
    if (sequence)
    {
        for (int next = slot + 1; next < kSlotCount; ++next)
        {
            if (SlotShown(next) && kSlots[next].kind != SlotKind::Motion &&
                kSlots[next].kind != SlotKind::StickModifier)
            {
                Listen(next, true);
                return;
            }
        }
    }
    ShowBindings();
}
