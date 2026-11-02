#pragma once
#include <SDL3/SDL.h>
#include <string>

struct Shortcut
{
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    bool operator==(const Shortcut & other) const { return key == other.key && ctrl == other.ctrl && alt == other.alt && shift == other.shift; }
    bool Matches(const SDL_KeyboardEvent & event) const;
    std::string Label() const;
    std::string Text() const;
    static Shortcut Parse(const std::string & text);
    static Shortcut Of(const SDL_KeyboardEvent & event);
};

bool CommandKeyboard();
