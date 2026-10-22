#include "volume_control.h"
#include "rml_events.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <cmath>
#include <cstdlib>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/ui_preferences.h>
#include <string>

namespace
{
constexpr int kNormalVolume = 100;
constexpr int kSnapRange = 5;

bool InSnapRange(int volume)
{
    return std::abs(volume - kNormalVolume) <= kSnapRange;
}

Rml::Element * Part(Rml::Element * root, const char * className)
{
    Rml::ElementList found;
    root->GetElementsByClassName(found, className);
    return found.empty() ? nullptr : found.front();
}
} // namespace

VolumeControl::VolumeControl(Rml::Element * root, std::function<void()> changed) :
    m_root(root),
    m_changed(std::move(changed))
{
    if (m_root == nullptr)
    {
        return;
    }
    m_root->SetInnerRML("<div class=\"volume-toggle\" title=\"Mute\"><img class=\"volume-icon\" src=\"images/volume.png\" /><div class=\"volume-text\" /></div>"
                        "<div class=\"volume-range\"><div class=\"volume-normal-mark\" />"
                        "<input class=\"range volume-slider\" type=\"range\" min=\"0\" max=\"200\" step=\"1\" value=\"100\" /></div>");

    UiEvents::Listen(Part(m_root, "volume-toggle"), "click", [this](Rml::Event &) {
        UiPreferences::SetStatusBool(NXOsSetting::AudioMuted, !SettingsStore::GetInstance().GetBool(NXOsSetting::AudioMuted));
        Show();
        m_changed();
    });
    // 100% attracts the slider while it is held, once it has been away from
    // it: grabbed at 100%, it leaves freely for 98%, and is caught again only
    // after going further than that. The press is seen before the slider acts
    // on it, so the volume read here is the one before a click on the track.
    Rml::Element * slider = Part(m_root, "volume-slider");
    UiEvents::ListenFirst(slider, "mousedown", [this](Rml::Event &) {
        m_held = true;
        m_snaps = !InSnapRange(SettingsStore::GetInstance().GetInt(NXOsSetting::AudioVolume));
    });
    UiEvents::Listen(m_root->GetOwnerDocument(), "mouseup", [this](Rml::Event &) { m_held = false; });
    UiEvents::Listen(slider, "dragend", [this](Rml::Event &) { m_held = false; });
    UiEvents::Listen(slider, "change", [this](Rml::Event & event) {
        // The volume is this control's: the document it is in has no change of its own to see.
        event.StopPropagation();
        SetFromSlider(static_cast<int>(std::lround(event.GetParameter<float>("value", static_cast<float>(kNormalVolume)))));
    });
    Show();
}

void VolumeControl::SetFromSlider(int volume)
{
    // Keys and the wheel move the slider without holding it: no attraction.
    if (m_held)
    {
        if (!InSnapRange(volume))
        {
            m_snaps = true;
        }
        else if (m_snaps)
        {
            volume = kNormalVolume;
        }
    }
    auto & store = SettingsStore::GetInstance();
    if (store.GetInt(NXOsSetting::AudioVolume) != volume || !store.HasControlOverride(NXOsSetting::AudioVolume))
    {
        UiPreferences::SetStatusInt(NXOsSetting::AudioVolume, volume);
        Show();
        m_changed();
    }
}

void VolumeControl::Show()
{
    if (m_root == nullptr)
    {
        return;
    }
    auto & store = SettingsStore::GetInstance();
    const int volume = store.GetInt(NXOsSetting::AudioVolume);
    const bool muted = store.GetBool(NXOsSetting::AudioMuted);
    const Rml::String text = std::to_string(volume) + "%";
    Rml::Element * label = Part(m_root, "volume-text");
    if (label != nullptr && label->GetInnerRML() != text)
    {
        label->SetInnerRML(text);
    }
    if (Rml::Element * toggle = Part(m_root, "volume-toggle"))
    {
        const std::string hint = muted ? Tr("Muted: click to hear the games again") : Tr("Mute");
        if (toggle->GetAttribute<Rml::String>("title", "") != hint)
        {
            toggle->SetAttribute("title", hint);
        }
    }
    if (Rml::Element * icon = Part(m_root, "volume-icon"))
    {
        const char * image = muted ? "images/volume-muted.png" : "images/volume.png";
        if (icon->GetAttribute<Rml::String>("src", "") != image)
        {
            icon->SetAttribute("src", image);
        }
    }
    if (Rml::Element * slider = Part(m_root, "volume-slider"))
    {
        const Rml::String value = std::to_string(volume);
        if (slider->GetAttribute<Rml::String>("value", "") != value)
        {
            slider->SetAttribute("value", value);
        }
    }
}
