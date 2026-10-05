#pragma once
#include <sciter_element.h>
#include <sciter_handler.h>
#include <cstdint>

class VolumeSliderDoubleClickSink final : public IDoubleClickSink
{
public:
    bool OnDoubleClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/) override
    {
        // A native macOS double-click can leave the slider tracking the mouse.
        SciterElement(element).ReleaseCapture();
        return true;
    }
};

inline void InitializeVolumeSlider(ISciterUI & sciterUI, SciterElement slider)
{
    static VolumeSliderDoubleClickSink doubleClickSink;
    sciterUI.AttachHandler(slider, IID_IDBLCLICKSINK, &doubleClickSink);

    SciterElement marker = slider.GetParent().FindFirst(".volume-normal-marker");
    if (marker.IsValid())
    {
        marker.SetValue(SciterValue(100));
    }
}

inline void SnapVolumeSliderToNormal(SciterElement slider, uint64_t reason)
{
    constexpr uint64_t mouseClickReason = 0; // Sciter BY_MOUSE_CLICK.
    const SciterValue value = slider.GetValue();
    // Keep keyboard steps precise and only snap mouse adjustments near 100%.
    if (reason == mouseClickReason && value.isInt() &&
        value.GetValueInt() >= 95 && value.GetValueInt() <= 105 && value.GetValueInt() != 100)
    {
        slider.SetValue(SciterValue(100));
    }
}
