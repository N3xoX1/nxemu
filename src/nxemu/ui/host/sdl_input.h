#pragma once

#include <SDL.h>
#include <nxemu-module-spec/base.h>
#include <string>

nxinterface IOperatingSystem;

// Forward physical keyboard events to the emulator using its HID key indices.
void ForwardKeyboardEvent(const SDL_KeyboardEvent& event, IOperatingSystem& os);

// Forwarded keys are known by their place, as the virtual key code of the key
// there on a US keyboard. What to call the key with this code: what the
// keyboard in use prints on it; empty when it has no name.
std::string VirtualKeyName(int virtualKey);
