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

class GameDetailsPanel
{
public:
    using Fact = std::pair<std::string, std::string>;

    GameDetailsPanel(Rml::Element * panel, std::function<void(size_t)> onToggleAddOn);

    void Clear();
    void ShowGame(const std::string & title, const std::string & iconSource, const std::vector<Fact> & facts);
    void ShowDetails(const GameDetails * details, bool locked);

private:
    GameDetailsPanel(const GameDetailsPanel &) = delete;
    GameDetailsPanel & operator=(const GameDetailsPanel &) = delete;

    void SetRml(const char * id, const std::string & rml);
    void SetLocked(const char * id, bool locked, const std::string & lockedHint, const std::string & hint);

    Rml::Element * m_panel;
};
