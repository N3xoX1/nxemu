#include "tooltip.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/StringUtilities.h>
#include <SDL.h>
#include <algorithm>
#include <nxemu/ui/i18n/translation.h>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr uint64_t kRestMs = 450;
constexpr uint64_t kFollowMs = 400;
constexpr float kGap = 8.0f;
constexpr float kEdge = 6.0f;

std::string ClippedText(Rml::Element * element)
{
    if (element->GetNumChildren() == 0 || element->GetComputedValues().overflow_x() != Rml::Style::Overflow::Hidden)
    {
        return {};
    }
    std::string text;
    for (int index = 0; index < element->GetNumChildren(); ++index)
    {
        const Rml::ElementText * part = rmlui_dynamic_cast<Rml::ElementText *>(element->GetChild(index));
        if (part == nullptr)
        {
            return {};
        }
        text += part->GetText();
    }
    const bool clipped = element->GetScrollWidth() > element->GetClientWidth() + 0.5f || element->GetScrollHeight() > element->GetClientHeight() + 0.5f;
    return clipped ? text : std::string();
}
}

Tooltip::Tooltip(Rml::Context * context, const std::filesystem::path & assetDir) :
    m_context(context)
{
    m_document = Translation::LoadDocument(*m_context, assetDir / "tooltip.rml");
    if (m_document != nullptr)
    {
        m_tip = m_document->GetElementById("tip");
        m_document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    }
}

Tooltip::~Tooltip()
{
    if (m_document != nullptr)
    {
        m_document->Close();
    }
}

void Tooltip::Update(bool suppressed)
{
    if (suppressed)
    {
        Hide();
        m_target = nullptr;
        return;
    }
    if (m_tip == nullptr)
    {
        return;
    }
    Rml::Element * target = m_context->GetHoverElement();
    std::string text;
    for (; target != nullptr; target = target->GetParentNode())
    {
        text = target->GetAttribute<Rml::String>("title", "");
        if (text.empty() && target == m_context->GetHoverElement())
        {
            text = ClippedText(target);
        }
        if (!text.empty())
        {
            break;
        }
    }

    const uint64_t now = SDL_GetTicks64();
    if (target != m_target || text != m_text)
    {
        Hide();
        m_target = target;
        m_text = text;
        m_restingSince = now;
    }
    if (m_shown || m_target == nullptr)
    {
        return;
    }
    const int ownRest = target->GetAttribute<int>("data-title-delay", 0);
    const bool follows = ownRest == 0 && m_hiddenAt != 0 && now - m_hiddenAt < kFollowMs;
    if (follows || now - m_restingSince >= (ownRest != 0 ? static_cast<uint64_t>(ownRest) : kRestMs))
    {
        m_tip->SetInnerRML(Rml::StringUtilities::EncodeRml(m_text));
        m_tip->SetClass("shown", true);
        Place(target);
        m_shown = true;
    }
}

void Tooltip::Place(Rml::Element * target)
{
    m_document->UpdateDocument();
    const float ratio = m_context->GetDensityIndependentPixelRatio();
    const float gap = kGap * ratio;
    const float edge = kEdge * ratio;
    const Rml::Vector2f window(static_cast<float>(m_context->GetDimensions().x), static_cast<float>(m_context->GetDimensions().y));
    const Rml::Vector2f size(m_tip->GetOffsetWidth(), m_tip->GetOffsetHeight());

    Rml::Vector2f position;
    if ((target->GetTagName() == "option" || target->IsClassSet("menu-item")) && target->GetParentNode() != nullptr)
    {
        Rml::Element * list = target->GetParentNode();
        const float left = list->GetAbsoluteOffset(Rml::BoxArea::Border).x;
        position.x = left + list->GetOffsetWidth() + gap;
        if (position.x + size.x > window.x - edge)
        {
            position.x = left - size.x - gap;
        }
        position.y = target->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    }
    else
    {
        const Rml::Vector2f offset = target->GetAbsoluteOffset(Rml::BoxArea::Border);
        position.x = offset.x;
        position.y = offset.y + target->GetOffsetHeight() + gap;
        if (position.y + size.y > window.y - edge)
        {
            position.y = offset.y - size.y - gap;
        }
    }
    position.x = std::max(edge, std::min(position.x, window.x - size.x - edge));
    position.y = std::max(edge, std::min(position.y, window.y - size.y - edge));
    m_tip->SetProperty(Rml::PropertyId::Left, Rml::Property(position.x, Rml::Unit::PX));
    m_tip->SetProperty(Rml::PropertyId::Top, Rml::Property(position.y, Rml::Unit::PX));
}

void Tooltip::Hide()
{
    if (m_shown)
    {
        m_shown = false;
        m_hiddenAt = SDL_GetTicks64();
        m_tip->SetClass("shown", false);
    }
}
