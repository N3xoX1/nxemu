#include "input_params.h"
#include <cstdlib>

namespace
{
constexpr char kPairSeparator = ',';
constexpr char kValueSeparator = ':';
constexpr char kEscape = '$';
constexpr const char * kEmpty = "[empty]";

std::string Escaped(const std::string & text)
{
    std::string result;
    for (const char c : text)
    {
        switch (c)
        {
        case kValueSeparator: result += "$0"; break;
        case kPairSeparator: result += "$1"; break;
        case kEscape: result += "$2"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string Unescaped(const std::string & text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == kEscape && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '2')
        {
            result += text[i + 1] == '0' ? kValueSeparator : text[i + 1] == '1' ? kPairSeparator : kEscape;
            ++i;
        }
        else
        {
            result += text[i];
        }
    }
    return result;
}
} // namespace

InputParams::InputParams(const std::string & text)
{
    if (text == kEmpty)
    {
        return;
    }
    for (size_t start = 0; start < text.size();)
    {
        size_t end = text.find(kPairSeparator, start);
        end = end == std::string::npos ? text.size() : end;
        const std::string pair = text.substr(start, end - start);
        const size_t split = pair.find(kValueSeparator);
        if (split != std::string::npos && pair.find(kValueSeparator, split + 1) == std::string::npos)
        {
            m_values[Unescaped(pair.substr(0, split))] = Unescaped(pair.substr(split + 1));
        }
        start = end + 1;
    }
}

InputParams InputParams::Take(IParamPackage * package)
{
    if (package == nullptr)
    {
        return InputParams();
    }
    InputParams params = Copy(*package);
    package->Release();
    return params;
}

InputParams InputParams::Copy(const IParamPackage & package)
{
    const char * text = package.Serialize();
    return InputParams(text != nullptr ? std::string(text) : std::string());
}

InputParams InputParams::Key(int keyCode)
{
    InputParams params;
    params.Set("engine", "keyboard");
    params.Set("code", keyCode);
    params.Set("toggle", 0);
    return params;
}

void InputParams::Set(const std::string & key, std::string value)
{
    m_values[key] = std::move(value);
}

std::string InputParams::Text() const
{
    if (m_values.empty())
    {
        return kEmpty;
    }
    std::string text;
    for (const auto & [key, value] : m_values)
    {
        text += Escaped(key) + kValueSeparator + Escaped(value) + kPairSeparator;
    }
    text.pop_back();
    return text;
}

bool InputParams::Has(const char * key) const
{
    return m_values.find(key) != m_values.end();
}

bool InputParams::GetBool(const char * key, bool default_value) const
{
    return GetInt(key, default_value ? 1 : 0) != 0;
}

int32_t InputParams::GetInt(const char * key, int32_t default_value) const
{
    const auto found = m_values.find(key);
    if (found == m_values.end())
    {
        return default_value;
    }
    char * end = nullptr;
    const long value = std::strtol(found->second.c_str(), &end, 10);
    return end != found->second.c_str() ? static_cast<int32_t>(value) : default_value;
}

float InputParams::GetFloat(const char * key, float default_value) const
{
    const auto found = m_values.find(key);
    if (found == m_values.end())
    {
        return default_value;
    }
    char * end = nullptr;
    const float value = std::strtof(found->second.c_str(), &end);
    return end != found->second.c_str() ? value : default_value;
}

const char * InputParams::GetString(const char * key, const char * default_value) const
{
    const auto found = m_values.find(key);
    return found != m_values.end() ? found->second.c_str() : default_value;
}

void InputParams::SetFloat(const char * key, float value)
{
    Set(key, std::to_string(value));
}

const char * InputParams::Serialize() const
{
    m_serialized = Text();
    return m_serialized.c_str();
}
