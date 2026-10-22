#pragma once
#include "menu_bar.h"
#include <RmlUi/Core/Types.h>
#include <functional>
#include <vector>

// A menu opened on demand beside the element it belongs to, such as the
// actions of a game card. It uses the menu bar's items and looks the same.
class PopupMenu
{
public:
    // anchor is an empty "menu-anchor" element, a child of the document's body.
    // onAction is called as MenuBar's: while RmlUi dispatches the click.
    // onRange receives the id and the new value of an item's range as its
    // slider moves; the menu stays open.
    PopupMenu(Rml::Element * anchor, std::function<void(int)> onAction, std::function<void(int, int)> onRange = {});

    // Opens the menu under the origin, their right edges aligned, or wherever
    // near it the document has room. Lays the document out: not to be called
    // from an event callback.
    void Show(const std::vector<MenuItem> & items, Rml::Element * origin);
    // Opens the menu at a point of the document, as the right button does.
    // The same restriction applies.
    void ShowAt(const std::vector<MenuItem> & items, Rml::Vector2f point);
    // Replaces the items of the open menu, which stays where it is.
    // The same restriction applies.
    void Update(const std::vector<MenuItem> & items);
    // The element the menu is open for, or nullptr.
    Rml::Element * Origin() const { return m_origin; }
    bool IsOpen() const { return m_open; }
    // Whether the mouse is over the open menu.
    bool Hovered() const;
    void Close();

private:
    PopupMenu(const PopupMenu &) = delete;
    PopupMenu & operator=(const PopupMenu &) = delete;

    Rml::Element * m_anchor;
    std::function<void(int)> m_onAction;
    std::function<void(int, int)> m_onRange;
    Rml::Element * m_origin = nullptr;
    bool m_open = false;
    Rml::Vector2f m_position;

    // Lays the menu out and returns its size.
    Rml::Vector2f Open(const std::vector<MenuItem> & items, Rml::Element * origin);
    void Place(Rml::Vector2f position, Rml::Vector2f size);
};
