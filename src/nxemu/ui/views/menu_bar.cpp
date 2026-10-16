#include "menu_bar.h"
#include "rml_events.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <cmath>

namespace
{
std::string LabelRml(const std::string & label)
{
    std::string text;
    for (const char character : label)
    {
        if (character != '&')
        {
            text += character;
        }
    }
    return Rml::StringUtilities::EncodeRml(text);
}

// A popup is as wide as its longest item. Its anchor is what gets positioned:
// placed directly, the popup could be no wider than the title or the item
// that opens it.
void AppendPopup(std::string & rml, const std::vector<MenuItem> & items)
{
    rml += "<div class=\"menu-anchor\">" + MenuPopupRml(items) + "</div>";
}

void AppendItem(std::string & rml, const MenuItem & item)
{
    if (item.kind == MenuItem::Kind::Separator)
    {
        rml += "<div class=\"menu-separator\"/>";
        return;
    }

    const bool submenu = item.kind == MenuItem::Kind::Submenu;
    rml += "<div class=\"menu-item";
    rml += submenu ? " has-submenu" : "";
    rml += item.enabled ? "\"" : " disabled\"";
    if (!submenu && item.enabled && item.id != 0)
    {
        rml += " data-action=\"" + std::to_string(item.id) + "\"";
        rml += item.kind == MenuItem::Kind::Check ? " data-check=\"\"" : "";
        rml += item.stays ? " data-stay=\"\"" : "";
    }
    if (!item.hint.empty())
    {
        rml += " title=\"" + Rml::StringUtilities::EncodeRml(item.hint) + "\"";
    }
    rml += "><div class=\"menu-check";
    rml += item.kind == MenuItem::Kind::Check ? (item.checked ? " checkbox checked" : " checkbox") : "";
    rml += item.kind == MenuItem::Kind::Choice ? (item.checked ? " choice checked" : " choice") : "";
    rml += "\"/><div class=\"menu-label\">" + LabelRml(item.label) + "</div>";
    if (!item.tag.empty())
    {
        rml += "<div class=\"menu-tag\">" + Rml::StringUtilities::EncodeRml(item.tag) + "</div>";
    }
    if (submenu)
    {
        rml += "<div class=\"menu-arrow\">&gt;</div>";
        AppendPopup(rml, item.children);
    }
    else if (item.range)
    {
        const MenuItem::Range & range = *item.range;
        rml += std::string("<div class=\"menu-range") + (range.enabled ? "" : " off") + "\" title=\"" + Rml::StringUtilities::EncodeRml(range.hint) +
               "\"><input class=\"range\" type=\"range\" min=\"" + std::to_string(range.min) + "\" max=\"" + std::to_string(range.max) + "\" step=\"" +
               std::to_string(range.step) + "\" value=\"" + std::to_string(range.value) + "\" data-range=\"" + std::to_string(range.id) + "\" data-unit=\"" +
               Rml::StringUtilities::EncodeRml(range.unit) + "\"" + (range.enabled ? "" : " disabled") + "/><div class=\"menu-range-text\">" +
               std::to_string(range.value) + Rml::StringUtilities::EncodeRml(range.unit) + "</div></div>";
    }
    else
    {
        rml += "<div class=\"menu-shortcut\">" + Rml::StringUtilities::EncodeRml(item.shortcut) + "</div>";
    }
    rml += "</div>";
}
} // namespace

std::string MenuPopupRml(const std::vector<MenuItem> & items)
{
    bool checks = false;
    for (const MenuItem & item : items)
    {
        checks = checks || item.kind == MenuItem::Kind::Check || item.kind == MenuItem::Kind::Choice;
    }
    // Only a popup with items to tick keeps a column for the marks.
    std::string rml = checks ? "<div class=\"menu-popup has-checks\">" : "<div class=\"menu-popup\">";
    for (const MenuItem & item : items)
    {
        AppendItem(rml, item);
    }
    return rml + "</div>";
}

bool MenuRangeClick(Rml::Event & event)
{
    Rml::Element * range = UiEvents::ClosestWithClass(event.GetTargetElement(), "menu-range");
    return range != nullptr && !range->IsClassSet("off");
}

bool MenuToggleCheck(Rml::Element * item)
{
    if (!item->HasAttribute("data-check"))
    {
        return false;
    }
    if (Rml::Element * check = item->GetFirstChild())
    {
        check->SetClass("checked", !check->IsClassSet("checked"));
    }
    return true;
}

bool MenuItemStays(Rml::Element * item)
{
    return item->HasAttribute("data-stay");
}

bool MenuRangeChange(Rml::Event & event, int & id, int & value)
{
    Rml::Element * slider = event.GetTargetElement();
    if (!slider->HasAttribute("data-range"))
    {
        return false;
    }
    id = slider->GetAttribute<int>("data-range", 0);
    value = static_cast<int>(std::lround(event.GetParameter<float>("value", 0.0f)));
    if (Rml::Element * text = slider->GetNextSibling())
    {
        text->SetInnerRML(std::to_string(value) + Rml::StringUtilities::EncodeRml(slider->GetAttribute<Rml::String>("data-unit", "")));
    }
    return true;
}

MenuItem MenuItem::Action(int id, std::string label, std::string shortcut, bool enabled)
{
    MenuItem item;
    item.id = id;
    item.label = std::move(label);
    item.shortcut = std::move(shortcut);
    item.enabled = enabled;
    return item;
}

MenuItem MenuItem::Check(int id, std::string label, bool checked, std::string shortcut)
{
    MenuItem item = Action(id, std::move(label), std::move(shortcut));
    item.kind = Kind::Check;
    item.checked = checked;
    return item;
}

MenuItem MenuItem::Choice(int id, std::string label, bool chosen)
{
    MenuItem item = Action(id, std::move(label));
    item.kind = Kind::Choice;
    item.checked = chosen;
    return item;
}

MenuItem MenuItem::Slider(std::string label, Range range)
{
    MenuItem item;
    item.label = std::move(label);
    item.range = std::move(range);
    return item;
}

MenuItem MenuItem::Separator()
{
    MenuItem item;
    item.kind = Kind::Separator;
    return item;
}

MenuItem MenuItem::Submenu(std::string label, std::vector<MenuItem> children)
{
    MenuItem item;
    item.kind = Kind::Submenu;
    item.label = std::move(label);
    item.children = std::move(children);
    return item;
}

MenuBar::MenuBar(Rml::Element * root, std::function<void(int)> onAction, std::function<void(int, int)> onRange) :
    m_root(root),
    m_onAction(std::move(onAction)),
    m_onRange(std::move(onRange))
{
    UiEvents::Listen(m_root, "click", [this](Rml::Event & event) { OnClick(event); });
    UiEvents::Listen(m_root, "change", [this](Rml::Event & event) {
        int id = 0;
        int value = 0;
        if (m_openMenu != nullptr && m_onRange && MenuRangeChange(event, id, value))
        {
            m_onRange(id, value);
        }
    });
    UiEvents::Listen(m_root, "mouseover", [this](Rml::Event & event) { OnMouseOver(event); });
    UiEvents::Listen(m_root->GetOwnerDocument(), "mousedown", [this](Rml::Event & event) {
        if (!m_root->Contains(event.GetTargetElement()))
        {
            Close();
        }
    });
}

void MenuBar::SetMenus(const std::vector<MenuItem> & menus)
{
    Close();
    std::string rml;
    for (const MenuItem & menu : menus)
    {
        rml += "<div class=\"menu\"><div class=\"menu-title\">" + LabelRml(menu.label) + "</div>";
        AppendPopup(rml, menu.children);
        rml += "</div>";
    }
    m_root->SetInnerRML(rml);
}

void MenuBar::Close()
{
    if (m_openMenu != nullptr)
    {
        m_openMenu->SetClass("open", false);
        m_openMenu = nullptr;
    }
}

void MenuBar::Open(Rml::Element * menu)
{
    if (menu == m_openMenu)
    {
        return;
    }
    Close();
    m_openMenu = menu;
    m_openMenu->SetClass("open", true);
}

void MenuBar::OnClick(Rml::Event & event)
{
    Rml::Element * target = event.GetTargetElement();
    if (MenuRangeClick(event))
    {
        return;
    }
    if (Rml::Element * item = UiEvents::ClosestWithClass(target, "menu-item"))
    {
        if (item->HasAttribute("data-action"))
        {
            if (!MenuToggleCheck(item))
            {
                Close();
            }
            m_onAction(item->GetAttribute<int>("data-action", 0));
        }
        return;
    }
    if (UiEvents::ClosestWithClass(target, "menu-title") == nullptr)
    {
        return;
    }
    Rml::Element * menu = UiEvents::ClosestWithClass(target, "menu");
    if (menu == m_openMenu)
    {
        Close();
    }
    else
    {
        Open(menu);
    }
}

void MenuBar::OnMouseOver(Rml::Event & event)
{
    // Once a menu is open, moving along the bar switches menus without a click.
    Rml::Element * menu = UiEvents::ClosestWithClass(event.GetTargetElement(), "menu");
    if (m_openMenu != nullptr && menu != nullptr)
    {
        Open(menu);
    }
}
