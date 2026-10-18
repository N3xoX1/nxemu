#include "sdl_input.h"

#include <nxemu-module-spec/input.h>
#include <nxemu-module-spec/operating_system.h>

namespace {

int VirtualKey(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return 'A' + int(key - SDLK_a);
    if (key >= SDLK_0 && key <= SDLK_9)
        return int(key);
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return 0x70 + int(key - SDLK_F1);
    if (key >= SDLK_F13 && key <= SDLK_F24)
        return 0x7c + int(key - SDLK_F13);
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        return 0x61 + int(key - SDLK_KP_1);
    switch (key)
    {
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_RETURN: return 0x0d;
    case SDLK_ESCAPE: return 0x1b;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2d;
    case SDLK_DELETE: return 0x2e;
    case SDLK_SEMICOLON: return 0xba;
    case SDLK_EQUALS: return 0xbb;
    case SDLK_COMMA: return 0xbc;
    case SDLK_MINUS: return 0xbd;
    case SDLK_PERIOD: return 0xbe;
    case SDLK_SLASH: return 0xbf;
    case SDLK_BACKQUOTE: return 0xc0;
    case SDLK_LEFTBRACKET: return 0xdb;
    case SDLK_BACKSLASH: return 0xdc;
    case SDLK_RIGHTBRACKET: return 0xdd;
    case SDLK_QUOTE: return 0xde;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return 0x11;
    case SDLK_LALT:
    case SDLK_RALT: return 0x12;
    case SDLK_KP_0: return 0x60;
    case SDLK_KP_MULTIPLY: return 0x6a;
    case SDLK_KP_PLUS: return 0x6b;
    case SDLK_KP_MINUS: return 0x6d;
    case SDLK_KP_PERIOD: return 0x6e;
    case SDLK_KP_DIVIDE: return 0x6f;
    default: return 0;
    }
}

SDL_Keycode UsKeycode(SDL_Scancode scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return SDLK_a + int(scancode - SDL_SCANCODE_A);
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return SDLK_1 + int(scancode - SDL_SCANCODE_1);
    switch (scancode)
    {
    case SDL_SCANCODE_0: return SDLK_0;
    case SDL_SCANCODE_SEMICOLON: return SDLK_SEMICOLON;
    case SDL_SCANCODE_EQUALS: return SDLK_EQUALS;
    case SDL_SCANCODE_COMMA: return SDLK_COMMA;
    case SDL_SCANCODE_MINUS: return SDLK_MINUS;
    case SDL_SCANCODE_PERIOD: return SDLK_PERIOD;
    case SDL_SCANCODE_SLASH: return SDLK_SLASH;
    case SDL_SCANCODE_GRAVE: return SDLK_BACKQUOTE;
    case SDL_SCANCODE_LEFTBRACKET: return SDLK_LEFTBRACKET;
    case SDL_SCANCODE_BACKSLASH: return SDLK_BACKSLASH;
    case SDL_SCANCODE_RIGHTBRACKET: return SDLK_RIGHTBRACKET;
    case SDL_SCANCODE_APOSTROPHE: return SDLK_QUOTE;
    default: return SDL_SCANCODE_TO_KEYCODE(scancode);
    }
}

bool Printed(int virtualKey)
{
    return (virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9') || (virtualKey >= 0xba && virtualKey <= 0xc0) ||
           (virtualKey >= 0xdb && virtualKey <= 0xde);
}
} // namespace

void ForwardKeyboardEvent(const SDL_KeyboardEvent& event, IOperatingSystem& os)
{
    if (event.repeat)
        return;
    const auto scancode = static_cast<uint32_t>(event.keysym.scancode);
    if (scancode == 0 || scancode >= static_cast<uint32_t>(KeyboardKey::NumKeyboardKeys))
        return;
    const int virtual_key = VirtualKey(UsKeycode(event.keysym.scancode));
    if (event.type == SDL_KEYDOWN)
        os.KeyboardKeyPress(0, int(scancode), virtual_key);
    else if (event.type == SDL_KEYUP)
        os.KeyboardKeyRelease(0, int(scancode), virtual_key);
}

std::string VirtualKeyName(int virtualKey)
{
    if (Printed(virtualKey))
    {
        for (int scancode = SDL_SCANCODE_A; scancode <= SDL_SCANCODE_SLASH; ++scancode)
        {
            if (VirtualKey(UsKeycode(static_cast<SDL_Scancode>(scancode))) == virtualKey)
            {
                const char * name = SDL_GetKeyName(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode)));
                if (name != nullptr && name[0] != '\0')
                    return name;
                break;
            }
        }
    }
    if ((virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9'))
        return std::string(1, static_cast<char>(virtualKey));
    if (virtualKey >= 0x70 && virtualKey <= 0x87)
        return "F" + std::to_string(virtualKey - 0x70 + 1);
    if (virtualKey >= 0x60 && virtualKey <= 0x69)
        return "Num " + std::to_string(virtualKey - 0x60);
    switch (virtualKey)
    {
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0d: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x1b: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2d: return "Insert";
    case 0x2e: return "Delete";
    case 0x6a: return "Num *";
    case 0x6b: return "Num +";
    case 0x6d: return "Num -";
    case 0x6e: return "Num .";
    case 0x6f: return "Num /";
    case 0xba: return ";";
    case 0xbb: return "=";
    case 0xbc: return ",";
    case 0xbd: return "-";
    case 0xbe: return ".";
    case 0xbf: return "/";
    case 0xc0: return "`";
    case 0xdb: return "[";
    case 0xdc: return "\\";
    case 0xdd: return "]";
    case 0xde: return "'";
    default: return std::string();
    }
}
