#include "shell_open.h"

#include <common/std_string.h>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

bool ShellOpen(const char * target_utf8, const void * owner_window)
{
    if (target_utf8 == nullptr || target_utf8[0] == '\0')
    {
        return false;
    }

#ifdef _WIN32
    bool converted = false;
    const std::wstring wide_target = stdstr(target_utf8).ToUTF16(stdstr::CODEPAGE_UTF8, &converted);
    if (!converted || wide_target.empty())
    {
        return false;
    }
    const HWND owner = (HWND)owner_window;
    const HINSTANCE result = ShellExecuteW(owner, L"open", wide_target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    (void)owner_window;
    return false;
#endif
}
