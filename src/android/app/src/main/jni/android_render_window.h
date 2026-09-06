#pragma once

#include <nxemu-module-spec/base.h>

class AndroidRenderWindow final : public IRenderWindow
{
public:
    void * RenderSurface() const override;
    float PixelRatio() const override;
};
