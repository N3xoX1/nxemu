#include "std_string.h"
#include "platform.h"
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <Windows.h>
#endif

stdstr::stdstr()
{
}

stdstr::stdstr(const std::string & str) :
    std::string(str)
{
}

stdstr::stdstr(const stdstr & str) :
    std::string((const std::string &)str)
{
}

stdstr::stdstr(const char * str) :
    std::string(str ? str : "")
{
}

strvector stdstr::Tokenize(const char * delimiter) const
{
    strvector tokens;

    stdstr::size_type lastPos = find_first_not_of(delimiter, 0);
    stdstr::size_type pos = find_first_of(delimiter, lastPos);
    size_t delLen = strlen(delimiter);
    while (stdstr::npos != pos)
    {
        tokens.push_back(substr(lastPos, pos - lastPos));
        lastPos = pos + delLen;
        pos = find_first_of(delimiter, lastPos);
    }
    if (stdstr::npos != lastPos)
    {
        tokens.push_back(substr(lastPos));
    }
    return tokens;
}

strvector stdstr::Tokenize(char delimiter) const
{
    strvector tokens;

    stdstr::size_type lastPos = find_first_not_of(delimiter, 0);
    stdstr::size_type pos = find_first_of(delimiter, lastPos);
    while (stdstr::npos != pos)
    {
        tokens.push_back(substr(lastPos, pos - lastPos));
        lastPos = pos + 1;
        pos = find_first_of(delimiter, lastPos);
    }
    if (stdstr::npos != lastPos)
    {
        tokens.push_back(substr(lastPos));
    }
    return tokens;
}

void stdstr::Format(const char * strFormat, ...)
{
    va_list args;
    va_start(args, strFormat);
    ArgFormat(strFormat, args);
    va_end(args);
}

stdstr & stdstr::ToLower()
{
    std::transform(begin(), end(), begin(), (char (*)(int))tolower);
    return *this;
}

stdstr & stdstr::ToUpper()
{
    std::transform(begin(), end(), begin(), (char (*)(int))toupper);
    return *this;
}

stdstr & stdstr::Replace(const char search, const char replace)
{
    std::string & str = *this;
    std::string::size_type pos = str.find(search);
    while (pos != std::string::npos)
    {
        str.replace(pos, 1, &replace);
        pos = str.find(search, pos + 1);
    }
    return *this;
}

stdstr & stdstr::Replace(const char * search, const char replace)
{
    std::string & str = *this;
    std::string::size_type pos = str.find(search);
    size_t SearchSize = strlen(search);
    while (pos != std::string::npos)
    {
        str.replace(pos, SearchSize, &replace);
        pos = str.find(search, pos + 1);
    }
    return *this;
}

stdstr & stdstr::Replace(const std::string & search, const std::string & replace)
{
    std::string & str = *this;
    std::string::size_type pos = str.find(search);
    size_t SearchSize = search.size();
    while (pos != std::string::npos)
    {
        str.replace(pos, SearchSize, replace);
        pos = str.find(search, pos + replace.length());
    }
    return *this;
}

stdstr & stdstr::TrimLeft(const char * chars2remove)
{
    if (!empty())
    {
        std::string::size_type pos = find_first_not_of(chars2remove);
        if (pos != std::string::npos)
        {
            erase(0, pos);
        }
        else
        {
            erase(begin(), end());
        }
    }
    return *this;
}

stdstr & stdstr::TrimRight(const char * chars2remove)
{
    if (!empty())
    {
        std::string::size_type pos = find_last_not_of(chars2remove);
        if (pos != std::string::npos)
        {
            erase(pos + 1);
        }
        else
        {
            erase(begin(), end());
        }
    }
    return *this;
}

stdstr & stdstr::Trim(const char * chars2remove)
{
    if (!empty())
    {
        std::string::size_type pos = find_first_not_of(chars2remove);
        if (pos != std::string::npos)
        {
            erase(0, pos);
        }
        else
        {
            erase(begin(), end());
        }

        pos = find_last_not_of(chars2remove);
        if (pos != std::string::npos)
        {
            erase(pos + 1);
        }
        else
        {
            erase(begin(), end());
        }
    }
    return *this;
}

#ifdef _WIN32
stdstr & stdstr::FromUTF16(const wchar_t * utf16Source, bool * success)
{
    bool converted = false;

    if (utf16Source == nullptr)
    {
        *this = "";
        converted = true;
    }
    else if (wcslen(utf16Source) > 0)
    {
        uint32_t needed = WideCharToMultiByte(CODEPAGE_UTF8, 0, utf16Source, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 0)
        {
            char * buf = (char *)alloca(needed + 1);
            if (buf != nullptr)
            {
                memset(buf, 0, needed + 1);

                needed = WideCharToMultiByte(CODEPAGE_UTF8, 0, utf16Source, -1, buf, needed, nullptr, nullptr);
                if (needed)
                {
                    *this = buf;
                    converted = true;
                }
            }
        }
    }
    if (success)
    {
        *success = converted;
    }
    return *this;
}

std::wstring stdstr::ToUTF16(unsigned int codePage, bool * success) const
{
    bool converted = false;
    std::wstring res;

    DWORD needed = MultiByteToWideChar(codePage, 0, this->c_str(), (int)this->length(), nullptr, 0);
    if (needed > 0)
    {
        wchar_t * buf = (wchar_t *)alloca((needed + 1) * sizeof(wchar_t));
        if (buf != nullptr)
        {
            memset(buf, 0, (needed + 1) * sizeof(wchar_t));

            needed = MultiByteToWideChar(codePage, 0, this->c_str(), (int)this->length(), buf, needed);
            if (needed)
            {
                res = buf;
                converted = true;
            }
        }
    }
    if (success)
    {
        *success = converted;
    }
    return res;
}
#endif

void stdstr::ArgFormat(const char * strFormat, va_list & args)
{
#pragma warning(push)
#pragma warning(disable : 4996)

    size_t nlen = _vscprintf(strFormat, args) + 1;
    char * buffer = (char *)alloca(nlen * sizeof(char));
    buffer[nlen - 1] = 0;
    if (buffer != nullptr)
    {
        vsprintf(buffer, strFormat, args);
        *this = buffer;
    }
#pragma warning(pop)
}

stdstr_f::stdstr_f(const char * strFormat, ...)
{
    va_list args;
    va_start(args, strFormat);
    ArgFormat(strFormat, args);
    va_end(args);
}
