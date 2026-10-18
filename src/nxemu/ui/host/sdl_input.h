#pragma once

#include <SDL.h>
#include <nxemu-module-spec/base.h>
#include <string>

nxinterface IOperatingSystem;

void ForwardKeyboardEvent(const SDL_KeyboardEvent& event, IOperatingSystem& os);
std::string VirtualKeyName(int virtualKey);
