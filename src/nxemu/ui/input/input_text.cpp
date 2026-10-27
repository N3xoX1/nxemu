#include "input_text.h"
#include "input_params.h"
#include <nxemu/ui/host/input_service.h>
#include <nxemu/ui/host/sdl_input.h>
#include <nxemu/ui/i18n/translation.h>

namespace
{
const char * CommonName(ButtonNames name)
{
    switch (name)
    {
    case ButtonNames::ButtonLeft: return TrMark("Left");
    case ButtonNames::ButtonRight: return TrMark("Right");
    case ButtonNames::ButtonDown: return TrMark("Down");
    case ButtonNames::ButtonUp: return TrMark("Up");
    case ButtonNames::ButtonA: return "A";
    case ButtonNames::ButtonB: return "B";
    case ButtonNames::ButtonX: return "X";
    case ButtonNames::ButtonY: return "Y";
    case ButtonNames::ButtonPlus: return TrMark("Plus");
    case ButtonNames::ButtonMinus: return TrMark("Minus");
    case ButtonNames::ButtonHome: return TrMark("Home");
    case ButtonNames::ButtonCapture: return TrMark("Capture");
    case ButtonNames::ButtonStickL: return TrMark("Stick L");
    case ButtonNames::ButtonStickR: return TrMark("Stick R");
    case ButtonNames::TriggerL: return "L";
    case ButtonNames::TriggerZL: return "ZL";
    case ButtonNames::TriggerSL: return "SL";
    case ButtonNames::TriggerR: return "R";
    case ButtonNames::TriggerZR: return "ZR";
    case ButtonNames::TriggerSR: return "SR";
    case ButtonNames::TriggerZ: return "Z";
    case ButtonNames::ButtonStart: return TrMark("Start");
    case ButtonNames::L1: return "L1";
    case ButtonNames::L2: return "L2";
    case ButtonNames::L3: return "L3";
    case ButtonNames::R1: return "R1";
    case ButtonNames::R2: return "R2";
    case ButtonNames::R3: return "R3";
    case ButtonNames::Circle: return TrMark("Circle");
    case ButtonNames::Cross: return TrMark("Cross");
    case ButtonNames::Square: return TrMark("Square");
    case ButtonNames::Triangle: return TrMark("Triangle");
    case ButtonNames::Share: return TrMark("Share");
    case ButtonNames::Options: return TrMark("Options");
    case ButtonNames::Home: return TrMark("Home");
    case ButtonNames::Touch: return TrMark("Touch");
    case ButtonNames::ButtonMouseWheel: return TrMark("Wheel");
    case ButtonNames::ButtonBackward: return TrMark("Backward");
    case ButtonNames::ButtonForward: return TrMark("Forward");
    case ButtonNames::ButtonTask: return TrMark("Task");
    case ButtonNames::ButtonExtra: return TrMark("Extra");
    default: return nullptr;
    }
}

bool Is(const IParamPackage & params, const char * key, const char * value)
{
    return std::string(params.GetString(key, "")) == value;
}
} // namespace

std::string InputText::Binding(const HostInputService & inputs, const IParamPackage & params)
{
    if (!params.Has("engine"))
    {
        return std::string();
    }
    std::string text = params.GetBool("turbo", false) ? Tr("Turbo") + " " : std::string();
    text += params.GetBool("toggle", false) ? Tr("Toggle") + " " : std::string();
    text += params.GetBool("inverted", false) ? Tr("Not") + " " : std::string();

    if (Is(params, "engine", "keyboard"))
    {
        const std::string key = VirtualKeyName(params.GetInt("code", 0));
        return key.empty() ? std::string() : text + key;
    }
    const ButtonNames name = inputs.ButtonName(params);
    if (name == ButtonNames::Invalid)
    {
        // Saved controller bindings remain valid while the guest input
        // backend is not loaded. Show a meaningful fallback, not "Invalid".
        if (!inputs.Available())
        {
            return text + std::string(params.GetString("engine", "")) +
                   (params.Has("button") ? " #" + std::string(params.GetString("button", "")) : "");
        }
        return Tr("Invalid");
    }
    if (name == ButtonNames::Engine)
    {
        return params.GetString("engine", "");
    }
    const char * marked = CommonName(name);
    const std::string common = marked != nullptr ? Tr(marked) : std::string();
    if (params.Has("hat"))
    {
        return text + Tr("Hat %1", {!common.empty() ? common : params.GetString("direction", "")});
    }
    if (params.Has("axis"))
    {
        return text + Tr("Axis %1", {!common.empty() ? common : params.GetString("axis", "") + std::string(Is(params, "invert", "-") ? "-" : "+")});
    }
    if (params.Has("axis_x") && params.Has("axis_y") && params.Has("axis_z"))
    {
        return text + Tr("Axes %1, %2, %3", {params.GetString("axis_x", ""), params.GetString("axis_y", ""), params.GetString("axis_z", "")});
    }
    if (params.Has("motion"))
    {
        return text + Tr("Motion %1", {!common.empty() ? common : params.GetString("motion", "")});
    }
    if (params.Has("button"))
    {
        return text + (!common.empty() ? common : Tr("Button %1", {params.GetString("button", "")}));
    }
    return Tr("Unknown");
}

std::string InputText::StickDirection(const HostInputService & inputs, const IParamPackage & stick, const char * direction)
{
    if (!stick.Has("engine"))
    {
        return std::string();
    }
    if (Is(stick, "engine", "analog_from_button"))
    {
        return Binding(inputs, InputParams(stick.GetString(direction, "")));
    }
    if (!stick.Has("axis_x") || !stick.Has("axis_y"))
    {
        return Tr("Unknown");
    }
    const std::string name = direction;
    const bool horizontal = name == "left" || name == "right";
    const bool inverted = Is(stick, horizontal ? "invert_x" : "invert_y", "-");
    const bool positive = (name == "right" || name == "up") != inverted;
    return Tr("Axis %1", {stick.GetString(horizontal ? "axis_x" : "axis_y", "") + std::string(positive ? "+" : "-")});
}
