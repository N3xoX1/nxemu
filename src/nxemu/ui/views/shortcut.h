#pragma once
#include <SDL.h>
#include <string>

// A keyboard shortcut. "ctrl" is the platform's command modifier: it matches
// Control everywhere and also Command on macOS keyboards.
struct Shortcut
{
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    bool operator==(const Shortcut & other) const { return key == other.key && ctrl == other.ctrl && alt == other.alt && shift == other.shift; }
    bool Matches(const SDL_Keysym & keysym) const;
    // The shortcut as the keyboard of this computer names it.
    std::string Label() const;
    // The shortcut as the settings keep it, the same on every computer, and
    // back from it: no shortcut when the text is not one.
    std::string Text() const;
    static Shortcut Parse(const std::string & text);
    // What a key event asks for; no shortcut for a modifier alone.
    static Shortcut Of(const SDL_Keysym & keysym);
};

// Whether the keyboard commands with Command rather than Control.
bool CommandKeyboard();
