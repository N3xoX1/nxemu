#pragma once

#include <mutex>

#include <nxemu-module-spec/base.h>

struct ANativeWindow;

class AndroidRenderWindow final : public IRenderWindow
{
public:
    ~AndroidRenderWindow();

    void AttachSurface(ANativeWindow * native_window, float pixel_ratio);
    void ClearSurface();

    void * RenderSurface() const override;
    float PixelRatio() const override;

private:
    mutable std::mutex m_mutex;
    ANativeWindow * m_native_window = nullptr;
    float m_pixel_ratio = 1.0f;
};
