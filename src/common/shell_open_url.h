#pragma once

// Opens a URL or file URI with the system default handler (browser, etc.).
// owner_window is optional; on Windows this is an HWND passed to ShellExecuteW.
bool ShellOpenUrl(const char * url_utf8, const void * owner_window = nullptr);
