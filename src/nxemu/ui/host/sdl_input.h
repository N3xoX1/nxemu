#pragma once

#include <SDL.h>
#include <nxemu-module-spec/base.h>
#include <string>

nxinterface IOperatingSystem;

void ForwardKeyboardEvent(const SDL_KeyboardEvent& event, IOperatingSystem& os);
int KeyboardVirtualKey(const SDL_KeyboardEvent & event);
std::string VirtualKeyName(int virtualKey);
