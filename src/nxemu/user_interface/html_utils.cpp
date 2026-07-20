#include "html_utils.h"
#include <common/base64.h>
#include <common/std_string.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <fstream>
#include <vector>

namespace
{
const char * SniffImageMime(const uint8_t * data, size_t size)
{
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
    {
        return "image/png";
    }
    if (size >= 2 && data[0] == 0xff && data[1] == 0xd8)
    {
        return "image/jpeg";
    }
    if (size >= 3 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
    {
        return "image/gif";
    }
    return "image/jpeg";
}

} // namespace

std::string HtmlEscape(const std::string & text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string ImageDataUri(const uint8_t * data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return {};
    }
    return stdstr_f("data:%s;base64,%s", SniffImageMime(data, size), base64_encode(data, size).c_str());
}

std::string ImageDataUriFromFile(const char * path)
{
    if (path == nullptr || path[0] == '\0')
    {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }

    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ImageDataUri(data.data(), data.size());
}

void AttachClickHandler(ISciterUI & sciterUI, const SciterElement & element, IClickSink * sink)
{
    if (element.IsValid())
    {
        sciterUI.AttachHandler(element, IID_ICLICKSINK, sink);
    }
}

void SetElementEnabled(const SciterElement & root, const char * id, bool enabled)
{
    SciterElement el(root.GetElementByID(id));
    if (!el.IsValid())
    {
        return;
    }
    if (enabled)
    {
        el.SetState(0, SciterElement::STATE_DISABLED, true);
    }
    else
    {
        el.SetState(SciterElement::STATE_DISABLED, 0, true);
    }
}

void SetElementVisible(const SciterElement & root, const char * id, bool visible)
{
    SciterElement el(root.GetElementByID(id));
    if (!el.IsValid())
    {
        return;
    }
    el.SetStyleAttribute("display", visible ? "" : "none");
}
