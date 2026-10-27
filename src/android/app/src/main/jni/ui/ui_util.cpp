#include "ui_util.h"
#include "settings/ui_identifiers.h"
#include "settings/ui_settings.h"

#include <nxemu-core/i18n/translation.h>
#include <nxemu-core/settings/settings.h>

namespace UiUtil
{
std::string Escape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

std::string UrlDecode(std::string_view text)
{
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2])))
        {
            decoded += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        }
        else
        {
            decoded += text[i];
        }
    }
    return decoded;
}

std::string ShortPath(const std::string & uri)
{
    const std::string decoded = UrlDecode(uri);
    const size_t last = decoded.rfind('/');
    return last == std::string::npos ? decoded : decoded.substr(last + 1);
}

std::string FormatGameFolder(const std::string & uri)
{
    const std::string decoded = UrlDecode(uri);
    constexpr std::string_view prefix = "content://com.android.externalstorage.documents/tree/";
    if (!decoded.starts_with(prefix))
    {
        return decoded;
    }
    const std::string path = decoded.substr(prefix.size());
    const size_t colon = path.find(':');
    const std::string volume = path.substr(0, colon);
    const std::string folder = colon == std::string::npos ? std::string() : path.substr(colon + 1);
    if (volume == "primary")
    {
        return Tr("Internal storage") + " / " + folder;
    }
    return volume + " / " + folder;
}

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

JsonValue ParseJson(const std::string & text)
{
    JsonValue value;
    if (!JsonReader().Parse(text.data(), text.data() + text.size(), value))
    {
        return JsonValue();
    }
    return value;
}

std::vector<std::string> StringArray(const JsonValue & value)
{
    std::vector<std::string> items;
    if (!value.isArray())
    {
        return items;
    }
    for (uint32_t i = 0; i < value.size(); i++)
    {
        items.push_back(value[i].asString());
    }
    return items;
}

bool ReadBool(const char * key)
{
    return SettingsStore::GetInstance().GetBool(key);
}

int ReadInt(const char * key)
{
    return SettingsStore::GetInstance().GetInt(key);
}

std::string ReadString(const char * key)
{
    const char * value = SettingsStore::GetInstance().GetString(key);
    return value != nullptr ? value : "";
}

void SetBool(const char * key, bool value)
{
    SettingsStore::GetInstance().SetBool(key, value);
}

void SetInt(const char * key, int value)
{
    SettingsStore::GetInstance().SetInt(key, value);
}

void SetString(const char * key, const std::string & value)
{
    SettingsStore::GetInstance().SetString(key, value.c_str());
}

void SaveSettings()
{
    SaveUISetting();
    SettingsStore::GetInstance().Save();
}

std::vector<std::string> ConfiguredFolders()
{
    return StringArray(ParseJson(UiUtil::ReadString(NXUISetting::GameDirectories)));
}
} // namespace UiUtil
