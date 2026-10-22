#pragma once
#include <map>
#include <nxemu-module-spec/operating_system.h>
#include <string>
#include <utility>

// What an input is bound to, as the input module describes it: an engine and
// what it needs to find the button, the axis or the key. The interface keeps
// its own copy to read and change one, and gives it back to the module.
class InputParams final : public IParamPackage
{
public:
    InputParams() = default;
    InputParams(const InputParams &) = default;
    InputParams(InputParams &&) = default;
    // An MSVC interface has no assignment operator, so only the values are assigned.
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
    // From the text Text() gives.
    explicit InputParams(const std::string & text);
    // A copy of a package the module made for the caller, which is released.
    static InputParams Take(IParamPackage * package);
    static InputParams Copy(const IParamPackage & package);
    // The keyboard key with the virtual key code.
    static InputParams Key(int keyCode);

    // Bound to nothing.
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
    mutable std::string m_serialized; // what Serialize() last returned
};
