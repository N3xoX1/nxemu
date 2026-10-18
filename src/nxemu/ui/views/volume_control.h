#pragma once
#include <functional>

namespace Rml
{
class Element;
} // namespace Rml

class VolumeControl final
{
public:
    VolumeControl(Rml::Element * root, std::function<void()> changed);

    void Show();

private:
    VolumeControl(const VolumeControl &) = delete;
    VolumeControl & operator=(const VolumeControl &) = delete;

    void SetFromSlider(int volume);

    Rml::Element * const m_root;
    const std::function<void()> m_changed;
    bool m_held = false;
    bool m_snaps = false;
};
