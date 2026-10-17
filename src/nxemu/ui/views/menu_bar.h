#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
class Element;
class Event;
} // namespace Rml

struct MenuItem
{
    enum class Kind
    {
        Action,
        Check,  // a box ticked on its own: the menu stays open
        Choice, // one of options excluding each other: choosing it closes the menu, unless it stays
        Separator,
        Submenu,
    };

    static MenuItem Action(int id, std::string label, std::string shortcut = {}, bool enabled = true);
    static MenuItem Check(int id, std::string label, bool checked, std::string shortcut = {});
    static MenuItem Choice(int id, std::string label, bool chosen);
    static MenuItem Separator();
    static MenuItem Submenu(std::string label, std::vector<MenuItem> children);

    struct Range
    {
        int id = 0;
        int max = 0;
        int value = 0;
        bool enabled = true;
        std::string hint;
        int min = 0;
        int step = 1;
        std::string unit = "%"; // said after the value
    };
    static MenuItem Slider(std::string label, Range range);

    Kind kind = Kind::Action;
    int id = 0;
    std::string label; // '&' marks the mnemonic, as in native menus
    std::string shortcut;
    std::string hint; // what the item does, shown when the mouse rests on it
    std::string tag;  // a word beside the label, in smaller text
    bool checked = false;
    bool enabled = true;
    bool stays = false; // a popup stays open once it is chosen, to compare it with the others
    std::optional<Range> range;
    std::vector<MenuItem> children;
};

std::string MenuPopupRml(const std::vector<MenuItem> & items);
bool MenuRangeClick(Rml::Event & event);
bool MenuToggleCheck(Rml::Element * item);
bool MenuItemStays(Rml::Element * item);
bool MenuRangeChange(Rml::Event & event, int & id, int & value);

class MenuBar
{
public:
    MenuBar(Rml::Element * root, std::function<void(int)> onAction, std::function<void(int, int)> onRange = {});

    void SetMenus(const std::vector<MenuItem> & menus);
    bool IsOpen() const { return m_openMenu != nullptr; }
    void Close();

private:
    MenuBar(const MenuBar &) = delete;
    MenuBar & operator=(const MenuBar &) = delete;

    void Open(Rml::Element * menu);
    void OnClick(Rml::Event & event);
    void OnMouseOver(Rml::Event & event);

    Rml::Element * m_root;
    std::function<void(int)> m_onAction;
    std::function<void(int, int)> m_onRange;
    Rml::Element * m_openMenu = nullptr;
};
