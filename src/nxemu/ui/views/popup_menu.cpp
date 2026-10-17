#include "popup_menu.h"
#include "rml_events.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <string>

namespace
{
constexpr float kGap = 4.0f;    // between the origin and the menu, in dp
constexpr float kMargin = 6.0f; // kept free along the document's edges, in dp
} // namespace

PopupMenu::PopupMenu(Rml::Element * anchor, std::function<void(int)> onAction, std::function<void(int, int)> onRange) :
    m_anchor(anchor),
    m_onAction(std::move(onAction)),
    m_onRange(std::move(onRange))
{
    UiEvents::Listen(m_anchor, "click", [this](Rml::Event & event) {
        if (MenuRangeClick(event))
        {
            return;
        }
        Rml::Element * item = UiEvents::ClosestWithClass(event.GetTargetElement(), "menu-item");
        if (item != nullptr && item->HasAttribute("data-action"))
        {
            if (!MenuToggleCheck(item) && !MenuItemStays(item))
            {
                Close();
            }
            m_onAction(item->GetAttribute<int>("data-action", 0));
        }
    });
    UiEvents::Listen(m_anchor, "change", [this](Rml::Event & event) {
        int id = 0;
        int value = 0;
        if (m_open && m_onRange && MenuRangeChange(event, id, value))
        {
            m_onRange(id, value);
        }
    });
    Rml::ElementDocument * document = m_anchor->GetOwnerDocument();
    UiEvents::Listen(document, "mousedown", [this](Rml::Event & event) {
        Rml::Element * target = event.GetTargetElement();
        if (m_open && !m_anchor->Contains(target) && (m_origin == nullptr || !m_origin->Contains(target)))
        {
            Close();
        }
    });
    UiEvents::Listen(document, "mousescroll", [this](Rml::Event &) { Close(); });
}

void PopupMenu::Show(const std::vector<MenuItem> & items, Rml::Element * origin)
{
    const Rml::Vector2f size = Open(items, origin);
    Rml::ElementDocument * document = m_anchor->GetOwnerDocument();
    const float density = document->GetContext()->GetDensityIndependentPixelRatio();
    const float gap = kGap * density;
    Rml::Vector2f position(origin->GetAbsoluteLeft() + origin->GetOffsetWidth() - size.x, origin->GetAbsoluteTop() + origin->GetOffsetHeight() + gap);
    if (position.y + size.y + kMargin * density > document->GetOffsetHeight())
    {
        position.y = origin->GetAbsoluteTop() - gap - size.y;
    }
    Place(position, size);
}

void PopupMenu::ShowAt(const std::vector<MenuItem> & items, Rml::Vector2f point)
{
    const Rml::Vector2f size = Open(items, nullptr);
    Rml::ElementDocument * document = m_anchor->GetOwnerDocument();
    const float margin = kMargin * document->GetContext()->GetDensityIndependentPixelRatio();
    if (point.y + size.y + margin > document->GetOffsetHeight())
    {
        point.y -= size.y;
    }
    Place(point, size);
}

void PopupMenu::Update(const std::vector<MenuItem> & items)
{
    if (!m_open)
    {
        return;
    }
    m_anchor->RemoveProperty(Rml::PropertyId::Width);
    m_anchor->SetInnerRML(MenuPopupRml(items));
    m_anchor->GetOwnerDocument()->UpdateDocument();
    Rml::Element * popup = m_anchor->GetFirstChild();
    Place(m_position, {popup->GetOffsetWidth(), popup->GetOffsetHeight()});
}

Rml::Vector2f PopupMenu::Open(const std::vector<MenuItem> & items, Rml::Element * origin)
{
    Close();
    m_open = true;
    m_origin = origin;
    if (m_origin != nullptr)
    {
        m_origin->SetClass("open", true);
    }
    m_anchor->RemoveProperty(Rml::PropertyId::Width);
    m_anchor->SetInnerRML(MenuPopupRml(items));
    m_anchor->SetClass("open", true);

    m_anchor->GetOwnerDocument()->UpdateDocument();
    Rml::Element * popup = m_anchor->GetFirstChild();
    return {popup->GetOffsetWidth(), popup->GetOffsetHeight()};
}

void PopupMenu::Place(Rml::Vector2f position, Rml::Vector2f size)
{
    Rml::ElementDocument * document = m_anchor->GetOwnerDocument();
    const float margin = kMargin * document->GetContext()->GetDensityIndependentPixelRatio();
    const float left = std::max(margin, std::min(position.x, document->GetOffsetWidth() - size.x - margin));
    const float top = std::max(margin, std::min(position.y, document->GetOffsetHeight() - size.y - margin));
    m_position = {left, top};
    m_anchor->SetProperty(Rml::PropertyId::Left, Rml::Property(left, Rml::Unit::PX));
    m_anchor->SetProperty(Rml::PropertyId::Top, Rml::Property(top, Rml::Unit::PX));
    m_anchor->SetProperty(Rml::PropertyId::Width, Rml::Property(size.x, Rml::Unit::PX));
}

bool PopupMenu::Hovered() const
{
    Rml::Element * hover = m_anchor->GetContext()->GetHoverElement();
    return m_open && hover != nullptr && m_anchor->Contains(hover);
}

void PopupMenu::Close()
{
    if (!m_open)
    {
        return;
    }
    m_open = false;
    if (m_origin != nullptr)
    {
        m_origin->SetClass("open", false);
        m_origin = nullptr;
    }
    m_anchor->SetClass("open", false);
}
