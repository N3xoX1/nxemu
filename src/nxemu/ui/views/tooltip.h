#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
} // namespace Rml

// The explanation of what the mouse rests on: the "title" of the element
// under it, its text when it is cut short for want of room, or the "title"
// of the nearest element around it that has one. One serves every
// document of the context.
class Tooltip final
{
public:
    Tooltip(Rml::Context * context, const std::filesystem::path & assetDir);
    ~Tooltip();

    // Advances by one frame.
    void Update(bool suppressed = false);

private:
    Tooltip(const Tooltip &) = delete;
    Tooltip & operator=(const Tooltip &) = delete;

    void Place(Rml::Element * target);
    void Hide();

    Rml::Context * const m_context;
    Rml::ElementDocument * m_document = nullptr;
    Rml::Element * m_tip = nullptr;
    const Rml::Element * m_target = nullptr; // what the mouse rests on; only compared, never used
    std::string m_text;                      // its explanation
    uint64_t m_restingSince = 0;             // in SDL ticks
    uint64_t m_hiddenAt = 0;
    bool m_shown = false;
};
