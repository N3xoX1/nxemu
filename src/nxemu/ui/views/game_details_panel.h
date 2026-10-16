#pragma once
#include <functional>
#include <nxemu/ui/library/game_details.h>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
class Element;
} // namespace Rml

// The panel beside the games that shows the selected one: its picture and
// what is known of it, and the updates, add-on content and mods the loader
// applies to it, each with a switch. The add-on content unfolds into its
// pieces when clicked.
class GameDetailsPanel
{
public:
    using Fact = std::pair<std::string, std::string>; // what it is, and its value

    // onToggleAddOn is given the index of an add-on in the details shown. It
    // is called while RmlUi dispatches the click: it must only take note.
    GameDetailsPanel(Rml::Element * panel, std::function<void(size_t)> onToggleAddOn, std::function<void(size_t)> onChangeOverride);

    // Shows that no game is selected.
    void Clear();
    // Facts without a value are left out.
    void ShowGame(const std::string & title, const std::string & iconSource, const std::vector<Fact> & facts);
    // Shows what opening the game's file told, or with nullptr that it is
    // still being read.
    void ShowDetails(const GameDetails * details);

private:
    GameDetailsPanel(const GameDetailsPanel &) = delete;
    GameDetailsPanel & operator=(const GameDetailsPanel &) = delete;

    void SetRml(const char * id, const std::string & rml);
    // What the button says of itself when the mouse rests on it: why it is
    // locked, or what it does.
    void SetLocked(const char * id, bool locked, const std::string & lockedHint, const std::string & hint);

    Rml::Element * m_panel;
    bool m_addOnContentOpen = false; // the pieces of the add-on content are listed
};
