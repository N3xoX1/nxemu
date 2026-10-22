#include "shortcut.h"
#include <cstring>

namespace
{
constexpr const char * kCtrl = "Ctrl+";
constexpr const char * kAlt = "Alt+";
constexpr const char * kShift = "Shift+";

bool IsModifier(SDL_Keycode key)
{
    switch (key)
    {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
    case SDLK_CAPSLOCK:
    case SDLK_NUMLOCKCLEAR:
    case SDLK_MODE: return true;
    default: return false;
    }
}

std::string Joined(const Shortcut & shortcut, const char * ctrl, const std::string & key)
{
    return shortcut.key == SDLK_UNKNOWN ? std::string() : std::string(shortcut.ctrl ? ctrl : "") + (shortcut.alt ? kAlt : "") + (shortcut.shift ? kShift : "") + key;
}

// Takes a modifier from the start of the text.
bool Take(std::string & text, const char * modifier)
{
    if (text.rfind(modifier, 0) != 0)
    {
        return false;
    }
    text.erase(0, std::strlen(modifier));
    return true;
}
} // namespace

bool CommandKeyboard()
{
    return std::strcmp(SDL_GetPlatform(), "Mac OS X") == 0;
}

bool Shortcut::Matches(const SDL_Keysym & keysym) const
{
    return key != SDLK_UNKNOWN && *this == Of(keysym);
}

std::string Shortcut::Label() const
{
    return Joined(*this, CommandKeyboard() ? "Cmd+" : kCtrl, key == SDLK_ESCAPE ? "Esc" : SDL_GetKeyName(key));
}

std::string Shortcut::Text() const
{
    return Joined(*this, kCtrl, SDL_GetKeyName(key));
}

Shortcut Shortcut::Parse(const std::string & text)
{
    Shortcut shortcut;
    std::string key = text;
    shortcut.ctrl = Take(key, kCtrl);
    shortcut.alt = Take(key, kAlt);
    shortcut.shift = Take(key, kShift);
    shortcut.key = SDL_GetKeyFromName(key.c_str());
    return shortcut.key != SDLK_UNKNOWN ? shortcut : Shortcut{};
}

Shortcut Shortcut::Of(const SDL_Keysym & keysym)
{
    if (IsModifier(keysym.sym))
    {
        return {};
    }
    return {keysym.sym, (keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0, (keysym.mod & KMOD_ALT) != 0, (keysym.mod & KMOD_SHIFT) != 0};
}
