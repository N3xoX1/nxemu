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
        Choice, // one of options excluding each other: choosing it closes the menu
        Separator,
        Submenu,
    };

    static MenuItem Action(int id, std::string label, std::string shortcut = {}, bool enabled = true);
    static MenuItem Check(int id, std::string label, bool checked, std::string shortcut = {});
    static MenuItem Choice(int id, std::string label, bool chosen);
    static MenuItem Separator();
    static MenuItem Submenu(std::string label, std::vector<MenuItem> children);

    // A value set with a slider on the item's line.
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
    // An item that is its range alone: choosing it does nothing.
    static MenuItem Slider(std::string label, Range range);

    Kind kind = Kind::Action;
    int id = 0;
    std::string label; // '&' marks the mnemonic, as in native menus
    std::string shortcut;
    std::string hint; // what the item does, shown when the mouse rests on it
    std::string tag;  // a word beside the label, in smaller text
    bool checked = false;
    bool enabled = true;
    std::optional<Range> range;
    std::vector<MenuItem> children;
};

// The markup of a popup holding the items, which the style sheet shows when
// its parent, a "menu-anchor" element, is open.
std::string MenuPopupRml(const std::vector<MenuItem> & items);
// Whether a click is the use of a range, and not its item being chosen.
bool MenuRangeClick(Rml::Event & event);
// Ticks or unticks the box of a clicked item that has one, which keeps its
// menu open. False for any other item.
bool MenuToggleCheck(Rml::Element * item);
// The id and the value of the range a "change" event moved, which the text
// beside it is made to say. False for the change of anything else.
bool MenuRangeChange(Rml::Event & event, int & id, int & value);

// A menu bar with drop-down menus, drawn by RmlUi inside the given element so
// it looks and behaves the same on every platform.
class MenuBar
{
public:
    // onAction receives the id of the chosen item, and onRange the id and
    // the new value of an item's range as its slider moves. They are called
    // while RmlUi is dispatching the event, so they must not rebuild the
    // menu synchronously.
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
