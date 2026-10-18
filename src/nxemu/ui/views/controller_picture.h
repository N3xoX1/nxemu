#pragma once
#include <cstddef>
#include <cstdint>
#include <nxemu-module-spec/operating_system.h>

struct ControllerPart
{
    enum class Kind
    {
        Button,
        Stick,
        StickView,
        StickCap,
    };

    Kind kind;
    uint32_t index;
    float x;
    float y;
    float width;
    float height;
};

struct ControllerPicture
{
    const char * image;
    float width;
    float height;
    const char * stickCaps[2];
    float stickTravel[2];
    const ControllerPart * parts;
    size_t partCount;
};

const ControllerPicture & PictureOf(NpadStyleIndex type);
