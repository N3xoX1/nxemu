#pragma once
#include <cstdint>
#include <string>

namespace Rml { class ElementDocument; }

class SettingsContext
{
public:
    void Global() {}
    bool IsGame() const { return false; }
    int32_t GetInt(const char * key) const;
    bool GetBool(const char * key) const;
    const char * GetString(const char * key) const;
    int32_t GetDefaultInt(const char * key) const;
    bool GetDefaultBool(const char * key) const;
    const char * GetDefaultString(const char * key) const;
    void SetInt(const char * key, int32_t value);
    void SetBool(const char * key, bool value);
    void SetString(const char * key, const char * value);
    bool InheritCommand(const std::string & /*command*/) { return false; }
    void InheritAll() {}
    void Decorate(Rml::ElementDocument * document) const;
};
