#pragma once
#include <functional>

namespace Rml
{
class Element;
} // namespace Rml

// The volume of games in the status bar: a button that mutes them, the level,
// and a slider that goes to 200% and that 100% attracts. What it sets is a
// choice of the status bar, which takes priority over the settings.
class VolumeControl final
{
public:
    // Fills `root` with the control. `changed` is called when the user
    // changes the volume or mutes it. The control lives as long as `root`.
    VolumeControl(Rml::Element * root, std::function<void()> changed);

    // Shows the volume of the settings.
    void Show();

private:
    VolumeControl(const VolumeControl &) = delete;
    VolumeControl & operator=(const VolumeControl &) = delete;

    void SetFromSlider(int volume);

    Rml::Element * const m_root;
    const std::function<void()> m_changed;
    // The slider is held with the mouse, and 100% attracts it.
    bool m_held = false;
    bool m_snaps = false;
};
