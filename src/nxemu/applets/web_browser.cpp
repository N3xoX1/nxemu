#include "web_browser.h"

#include <common/shell_open.h>

#include <string>
#include <string_view>

namespace
{

std::string FileUriFromLocalPath(std::string_view local_url_utf8)
{
    std::string uri{"file:///"};
    uri.reserve(local_url_utf8.size() + 8);
    for (const char c : local_url_utf8)
    {
        if (c == '\\')
        {
            uri.push_back('/');
        }
        else
        {
            uri.push_back(c);
        }
    }
    return uri;
}

void InvokeOpenResult(OpenWebPageFn callback, void * user_data, bool success, const char * last_url_utf8)
{
    if (callback == nullptr)
    {
        return;
    }
    callback(user_data, static_cast<uint32_t>(success ? WebExitReasonHost::EndButtonPressed : WebExitReasonHost::WindowClosed), last_url_utf8 != nullptr ? last_url_utf8 : "");
}

} // namespace

WebBrowserApplet::WebBrowserApplet() :
    m_hwnd(nullptr)
{
}

void WebBrowserApplet::AttachToWindow(const void * hwnd)
{
    m_hwnd = const_cast<void *>(hwnd);
}

void WebBrowserApplet::DetachWindow()
{
    m_hwnd = nullptr;
}

void WebBrowserApplet::Close()
{
}

void WebBrowserApplet::OpenLocalWebPage(const char * local_url_utf8, void * extract_user_data, ExtractRomFsFn extract_romfs, void * open_user_data, OpenWebPageFn open_callback) const
{
    if (extract_romfs != nullptr)
    {
        extract_romfs(extract_user_data);
    }

    const std::string local_path = local_url_utf8 != nullptr ? local_url_utf8 : "";
    if (local_path.empty())
    {
        InvokeOpenResult(open_callback, open_user_data, false, "");
        return;
    }

    const std::string file_uri = FileUriFromLocalPath(local_path);
    const bool success = ShellOpen(file_uri.c_str(), m_hwnd);
    InvokeOpenResult(open_callback, open_user_data, success, success ? file_uri.c_str() : "");
}

void WebBrowserApplet::OpenExternalWebPage(const char * external_url_utf8, void * user_data, OpenWebPageFn callback) const
{
    const std::string external_url = external_url_utf8 != nullptr ? external_url_utf8 : "";
    if (external_url.empty())
    {
        InvokeOpenResult(callback, user_data, false, "");
        return;
    }

    const bool success = ShellOpen(external_url.c_str(), m_hwnd);
    InvokeOpenResult(callback, user_data, success, success ? external_url.c_str() : "");
}
