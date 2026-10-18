#pragma once
#include <map>
#include <nxemu-module-spec/operating_system.h>
#include <string>
#include <utility>

class InputParams final : public IParamPackage
{
public:
    InputParams() = default;
    InputParams(const InputParams &) = default;
    InputParams(InputParams &&) = default;
    InputParams & operator=(const InputParams & other)
    {
        m_values = other.m_values;
        return *this;
    }
    InputParams & operator=(InputParams && other) noexcept
    {
        m_values = std::move(other.m_values);
        return *this;
    }
    explicit InputParams(const std::string & text);
    static InputParams Take(IParamPackage * package);
    static InputParams Copy(const IParamPackage & package);
    static InputParams Key(int keyCode);

    bool Unset() const { return !Has("engine"); }
    std::string Get(const char * key) const { return GetString(key, ""); }
    void Set(const std::string & key, std::string value);
    void Set(const std::string & key, int value) { Set(key, std::to_string(value)); }
    void Erase(const std::string & key) { m_values.erase(key); }
    std::string Text() const;

    // IParamPackage
    bool Has(const char * key) const override;
    bool GetBool(const char * key, bool default_value) const override;
    int32_t GetInt(const char * key, int32_t default_value) const override;
    float GetFloat(const char * key, float default_value) const override;
    const char * GetString(const char * key, const char * default_value) const override;
    void SetFloat(const char * key, float value) override;
    const char * Serialize() const override;
    void Release() override {}

private:
    std::map<std::string, std::string> m_values;
    mutable std::string m_serialized;
};
