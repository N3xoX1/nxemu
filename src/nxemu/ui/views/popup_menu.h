#pragma once
#include "menu_bar.h"
#include <RmlUi/Core/Types.h>
#include <functional>
#include <vector>

class PopupMenu
{
public:
    PopupMenu(Rml::Element * anchor, std::function<void(int)> onAction, std::function<void(int, int)> onRange = {});

    void Show(const std::vector<MenuItem> & items, Rml::Element * origin);
    void ShowAt(const std::vector<MenuItem> & items, Rml::Vector2f point);
    void Update(const std::vector<MenuItem> & items);
    Rml::Element * Origin() const { return m_origin; }
    bool IsOpen() const { return m_open; }
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

    Rml::Vector2f Open(const std::vector<MenuItem> & items, Rml::Element * origin);
    void Place(Rml::Vector2f position, Rml::Vector2f size);
};
