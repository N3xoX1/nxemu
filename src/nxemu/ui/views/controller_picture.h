#pragma once
#include <cstddef>
#include <cstdint>
#include <nxemu-module-spec/operating_system.h>

// A part of a controller in its picture: where it is, as fractions of the
// picture's width and height.
struct ControllerPart
{
    enum class Kind
    {
        Button,    // index: the button
        Stick,     // index: the stick, where it is on the controller
        StickView, // index: the stick, the circle its travel is shown in
        StickCap,  // index: the stick, the image of its cap, which moves with it
    };

    Kind kind;
    uint32_t index;
    float x;
    float y;
    float width;
    float height;
};

// The picture of a type of controller and what can be pointed at in it.
struct ControllerPicture
{
    const char * image; // in the interface's assets, without the caps of its sticks
    float width;        // of the image, in pixels
    float height;
    const char * stickCaps[2]; // the image of each stick's cap, or nullptr without the stick
    float stickTravel[2];      // how far a cap moves when its stick is pushed all the way, as a fraction of the width
    const ControllerPart * parts;
    size_t partCount;
};

const ControllerPicture & PictureOf(NpadStyleIndex type);
