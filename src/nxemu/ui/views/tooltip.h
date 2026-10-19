#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

class Tooltip final
{
public:
    Tooltip(Rml::Context * context, const std::filesystem::path & assetDir);
    ~Tooltip();

    void Update(bool suppressed = false);

private:
    Tooltip(const Tooltip &) = delete;
    Tooltip & operator=(const Tooltip &) = delete;

    void Place(Rml::Element * target);
    void Hide();

    Rml::Context * const m_context;
    Rml::ElementDocument * m_document = nullptr;
    Rml::Element * m_tip = nullptr;
    const Rml::Element * m_target = nullptr;
    std::string m_text;
    uint64_t m_restingSince = 0;
    uint64_t m_hiddenAt = 0;
    bool m_shown = false;
};
