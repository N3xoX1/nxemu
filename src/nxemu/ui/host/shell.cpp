#include "shell.h"
#include <SDL.h>
#include <cctype>

bool OpenUrl(const std::string & url)
{
    return !url.empty() && SDL_OpenURL(url.c_str()) == 0;
}

bool OpenDirectory(const std::string & utf8Path)
{
    if (utf8Path.empty())
    {
        return false;
    }
    static const char hex[] = "0123456789ABCDEF";
    std::string url = utf8Path.front() == '/' ? "file://" : "file:///";
    for (const char character : utf8Path)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || byte == '/' || byte == ':' || byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            url += character;
        }
        else if (byte == '\\')
        {
            url += '/';
        }
        else
        {
            url += '%';
            url += hex[byte >> 4];
            url += hex[byte & 0xf];
        }
    }
    return OpenUrl(url);
}
