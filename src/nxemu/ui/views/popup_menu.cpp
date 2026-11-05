#include "popup_menu.h"
#include "rml_events.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <string>
#include <utility>

namespace
{
constexpr float kGap = 4.0f;    // between the origin and the menu, in dp
constexpr float kMargin = 6.0f; // kept free along the document's edges, in dp

// Stop at the panel itself: its padding does not belong to the parent row.
Rml::Element * RowInPanel(Rml::Element * element)
{
    for (; element && !element->IsClassSet("menu-popup"); element = element->GetParentNode())
    {
        if (element->IsClassSet("menu-item")) return element;
    }
    return nullptr;
}

Rml::Element * SubmenuRow(Rml::Element * panel, int index)
{
    for (int i = 0; i < panel->GetNumChildren(); ++i)
    {
        auto * row = panel->GetChild(i);
        if (row->IsClassSet("has-submenu") && index-- == 0)
        {
            return row;
        }
    }
    return nullptr;
}

bool MovingTowardPanel(Rml::Element * row, Rml::Vector2f previous, Rml::Vector2f current)
{
    auto* panel = row->GetLastChild()->GetFirstChild();
    const float left = panel->GetAbsoluteLeft();
    const float right = left + panel->GetOffsetWidth();
    const float rowCenter = row->GetAbsoluteLeft() + row->GetOffsetWidth() / 2;
    const float edge = left >= rowCenter ? left : right;
    const float dx = current.x - previous.x;
    const float distance = edge - current.x;
    if (dx == 0 || distance / dx < 0) return false;
    // Project the pointer movement onto the edge facing the parent panel.
    const float y = current.y + distance / dx * (current.y - previous.y);
    const float top = panel->GetAbsoluteTop();
    return y >= top && y <= top + panel->GetOffsetHeight();
}
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
        Rml::Element * item = RowInPanel(event.GetTargetElement());
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
    UiEvents::Listen(document, "mousemove", [this](Rml::Event & event) {
        if (!m_open || !m_anchor->IsClassSet("graphics-popup")) return;
        const Rml::Vector2f position(event.GetParameter<float>("mouse_x", 0),
                                    event.GetParameter<float>("mouse_y", 0));
        const auto previous = std::exchange(m_previousPointer, position);
        auto* item = RowInPanel(event.GetTargetElement());
        if (!item || !m_anchor->Contains(item)) return;
        auto* parent = item->GetParentNode();
        for (int i = 0; i < parent->GetNumChildren(); ++i)
        {
            auto* sibling = parent->GetChild(i);
            if (sibling != item && sibling->IsClassSet("submenu-open") &&
                previous && MovingTowardPanel(sibling, *previous, position))
            {
                return;
            }
        }
        OpenSubmenu(item);
    });
    UiEvents::Listen(document, "mousedown", [this](Rml::Event & event) {
        Rml::Element * target = event.GetTargetElement();
        if (m_open && !m_anchor->Contains(target) && (m_origin == nullptr || !m_origin->Contains(target)))
        {
            Close();
        }
    });
    UiEvents::Listen(document, "mousescroll", [this](Rml::Event &) { Close(); });
}

void PopupMenu::OpenSubmenu(Rml::Element * item)
{
    const bool alreadyOpen = item->IsClassSet("submenu-open");
    // Keep the selected branch open while crossing the edge of a panel.
    // :hover alone can disappear for a frame between the parent row and
    // its absolutely positioned child, making the third panel unreachable.
    auto* parent = item->GetParentNode();
    for (int i = 0; i < parent->GetNumChildren(); ++i)
    {
        auto* sibling = parent->GetChild(i);
        sibling->SetClass("submenu-open", sibling == item && item->IsClassSet("has-submenu"));
    }
    if (alreadyOpen || !item->IsClassSet("has-submenu")) return;
    Rml::Element * childAnchor = item->GetLastChild();
    if (!childAnchor || !childAnchor->IsClassSet("menu-anchor")) return;
    Rml::Element * popup = childAnchor->GetFirstChild();
    if (!popup) return;
    auto* document = m_anchor->GetOwnerDocument();
    document->UpdateDocument();
    const float width = popup->GetOffsetWidth();
    const float height = popup->GetOffsetHeight();
    if (width <= 0 || height <= 0) return;
    // Replace the generic measurement width with the visible panel width.
    childAnchor->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::PX));
    const float margin = kMargin * document->GetContext()->GetDensityIndependentPixelRatio();
    const float parentLeft = item->GetAbsoluteLeft();
    const float parentTop = item->GetAbsoluteTop();
    float left = parentLeft + item->GetOffsetWidth();
    if (left + width > document->GetOffsetWidth() - margin) left = parentLeft - width;
    left = std::max(margin, std::min(left, document->GetOffsetWidth() - width - margin));
    const float top = std::max(margin, std::min(parentTop + item->GetOffsetHeight() - height,
        document->GetOffsetHeight() - height - margin));
    childAnchor->SetProperty("bottom", "auto");
    childAnchor->SetProperty(Rml::PropertyId::Left, Rml::Property(left - parentLeft, Rml::Unit::PX));
    childAnchor->SetProperty(Rml::PropertyId::Top, Rml::Property(top - parentTop, Rml::Unit::PX));
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
    // Count only submenu rows. Profile changes can insert or remove slider rows.
    std::vector<int> openPath;
    if (m_anchor->IsClassSet("graphics-popup"))
    {
        auto * panel = m_anchor->GetFirstChild();
        while (panel)
        {
            int index = 0;
            auto * row = SubmenuRow(panel, index);
            while (row && !row->IsClassSet("submenu-open"))
            {
                row = SubmenuRow(panel, ++index);
            }
            if (!row)
            {
                break;
            }
            openPath.push_back(index);
            panel = row->GetLastChild()->GetFirstChild();
        }
    }
    m_previousPointer.reset();
    m_anchor->RemoveProperty(Rml::PropertyId::Width);
    m_anchor->SetInnerRML(MenuPopupRml(items));
    m_anchor->GetOwnerDocument()->UpdateDocument();
    Rml::Element * popup = m_anchor->GetFirstChild();
    Place(m_position, {popup->GetOffsetWidth(), popup->GetOffsetHeight()});
    for (const int index : openPath)
    {
        auto * row = SubmenuRow(popup, index);
        if (!row)
        {
            break;
        }
        OpenSubmenu(row);
        popup = row->GetLastChild()->GetFirstChild();
    }
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
    m_previousPointer.reset();
    m_open = false;
    if (m_origin != nullptr)
    {
        m_origin->SetClass("open", false);
        m_origin = nullptr;
    }
    m_anchor->SetClass("open", false);
}
