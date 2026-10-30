// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstring>
#include <string_view>
#include "android_render_window.h"
#include <android/native_window.h>

namespace {
int calls = 0, error = 0;
float rate = -1.f;
ANativeWindow* hinted = nullptr;
std::string_view mode;
int32_t SetRate(ANativeWindow* window, float value, int8_t compatibility) {
    assert(compatibility == 0);
    hinted = window;
    rate = value;
    ++calls;
    return error;
}
int32_t SetStrategy(ANativeWindow* window, float value, int8_t compatibility, int8_t strategy) {
    assert(strategy == 0);
    return SetRate(window, value, compatibility);
}
}
void* TestAndroidSymbol(void*, const char* name) {
    if (mode == "missing") return nullptr;
    if (!std::strcmp(name, "ANativeWindow_setFrameRateWithChangeStrategy")) {
        return mode == "legacy" ? nullptr : reinterpret_cast<void*>(SetStrategy);
    }
    return reinterpret_cast<void*>(SetRate);
}

int main(int argc, char** argv) {
    mode = argc > 1 ? argv[1] : "strategy";
    ANativeWindow first, second;
    AndroidRenderWindow window;
    window.AttachSurface(&first, 1.f);
    error = mode == "rejected" ? -1 : 0;
    window.OnFramePresented(&first, 33'333'333);
    if (mode == "missing" || mode == "rejected") {
        const int attempts = calls;
        window.OnFramePresented(&first, 16'666'666);
        assert(calls == attempts); // Unsupported/rejected APIs do not retry every frame.
        window.ClearSurface();
        assert(first.releases == 1);
        return 0;
    }
    assert(calls == 1 && hinted == &first && std::fabs(rate - 30.f) < 0.001f);
    window.OnFramePresented(&first, 33'333'333);
    window.OnFramePresented(&first, 16'666'666);
    assert(calls == 1); // Unchanged targets and rapid changes do not switch the display.
    window.SetPaused(true);
    assert(calls == 2 && rate == 0.f);
    window.OnFramePresented(&first, 8'333'333);
    assert(calls == 2);
    window.SetPaused(false);
    window.OnFramePresented(&first, 8'333'333);
    assert(calls == 3 && std::fabs(rate - 120.f) < 0.001f);
    window.OnFramePresented(&first, 0); // Unlocked frame clears the hint immediately.
    assert(calls == 4 && rate == 0.f);
    window.OnFramePresented(&first, 16'666'666);
    window.AttachSurface(&second, 2.f);
    assert(calls == 6 && rate == 0.f && first.releases == 1);
    window.OnFramePresented(&first, 33'333'333); // Queued frame for a released surface.
    assert(calls == 6);
    window.OnFramePresented(&second, 16'666'666);
    assert(calls == 7 && hinted == &second);
    window.SuppressPresentation();
    assert(calls == 8 && rate == 0.f);
    window.OnFramePresented(&second, 16'666'666);
    assert(calls == 8);
    window.AttachSurface(&second, 1.f);
    window.OnFramePresented(&second, 16'666'666);
    window.ClearSurface();
    assert(calls == 10 && rate == 0.f && second.releases == 1);
    window.ClearSurface();
    assert(calls == 10 && second.releases == 1);
}
