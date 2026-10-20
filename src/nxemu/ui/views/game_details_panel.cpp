#include "game_details_panel.h"
#include "rml_events.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <nxemu/ui/i18n/translation.h>

namespace
{
std::string Encoded(const std::string & text)
{
    return Rml::StringUtilities::EncodeRml(text);
}

std::string SectionTitle(GameAddOn::Kind kind)
{
    switch (kind)
    {
    case GameAddOn::Kind::Update: return Tr("Updates");
    case GameAddOn::Kind::AddOnContent: return Tr("DLC");
    default: return Tr("Mods");
    }
}

size_t AddOnContentCount(const GameAddOn & addOn)
{
    return static_cast<size_t>(std::count(addOn.version.begin(), addOn.version.end(), ',')) + 1;
}
} // namespace

GameDetailsPanel::GameDetailsPanel(Rml::Element * panel, std::function<void(size_t)> onToggleAddOn) :
    m_panel(panel)
{
    UiEvents::Listen(m_panel->GetElementById("details-addons"), "click", [onToggleAddOn = std::move(onToggleAddOn)](Rml::Event & event) {
        Rml::Element * toggle = UiEvents::ClosestWithClass(event.GetTargetElement(), "switch");
        const int index = toggle != nullptr && !toggle->IsClassSet("locked") ? toggle->GetAttribute<int>("data-addon", -1) : -1;
        if (index >= 0)
        {
            onToggleAddOn(static_cast<size_t>(index));
        }
    });
}

void GameDetailsPanel::Clear()
{
    m_panel->SetClass("has-game", false);
}

void GameDetailsPanel::ShowGame(const std::string & title, const std::string & iconSource, const std::vector<Fact> & facts)
{
    m_panel->SetClass("has-game", true);
    SetRml("details-title", Encoded(title));
    if (Rml::Element * icon = m_panel->GetElementById("details-icon"))
    {
        if (iconSource.empty())
        {
            icon->RemoveAttribute("src");
        }
        else if (icon->GetAttribute<Rml::String>("src", "") != iconSource)
        {
            icon->SetAttribute("src", iconSource);
        }
        icon->SetClass("shown", !iconSource.empty());
    }
    std::string rml;
    for (const Fact & fact : facts)
    {
        if (!fact.second.empty())
        {
            rml += "<div class=\"details-fact\"><div class=\"details-fact-name\">" + Encoded(fact.first) + "</div><div class=\"details-fact-value\">" +
                   Encoded(fact.second) + "</div></div>";
        }
    }
    SetRml("details-facts", rml);
}

void GameDetailsPanel::ShowDetails(const GameDetails * details, bool locked)
{
    const bool known = details != nullptr;
    SetLocked("details-save", !known || details->saveDirectory.empty(), known ? Tr("This game has not saved anything yet") : std::string(), Tr("Opens the folder this game saves in"));
    SetLocked("details-cache", !known || details->cacheDirectory.empty(), known ? Tr("This game has no shader cache yet") : std::string(), Tr("Opens the folder of this game's shader cache"));
    SetLocked("details-mods", !known || details->modDirectory.empty(), {}, Tr("Opens the folder this game's mods go in"));
    if (!known)
    {
        SetRml("details-addons", {});
        return;
    }

    const std::string switchLock = locked ? " locked\" title=\"" + Encoded(Tr("Add-ons cannot change while a game is running")) : std::string();
    std::string rml;
    for (const GameAddOn::Kind kind : {GameAddOn::Kind::Update, GameAddOn::Kind::AddOnContent, GameAddOn::Kind::Mod})
    {
        std::string rows;
        size_t count = 0;
        for (size_t i = 0; i < details->addOns.size(); i++)
        {
            const GameAddOn & addOn = details->addOns[i];
            if (addOn.kind != kind)
            {
                continue;
            }
            std::string name = addOn.name;
            std::string version = addOn.version;
            if (kind == GameAddOn::Kind::AddOnContent)
            {
                const size_t pieces = AddOnContentCount(addOn);
                count += pieces;
                name = Tr("Add-on content");
                version = TrCount("%n item", "%n items", pieces);
            }
            else
            {
                count++;
            }
            rows += "<div class=\"addon-row\"><div class=\"addon-name\">" + Encoded(name) + "</div><div class=\"addon-version\">" + Encoded(version) +
                    "</div><div class=\"switch" + (addOn.enabled ? " on" : "") + switchLock + "\" data-addon=\"" + std::to_string(i) +
                    "\"><div class=\"switch-knob\"/></div></div>";
        }
        if (!rows.empty())
        {
            rml += "<div class=\"details-section\"><div class=\"details-section-title\">" + Encoded(SectionTitle(kind)) + " (" + std::to_string(count) +
                   ")</div><div class=\"details-section-line\"/></div>" + rows;
        }
    }
    if (rml.empty())
    {
        rml = "<div class=\"details-note\">" + Encoded(Tr("No update, DLC or mod found for this game.")) + "</div>";
    }
    SetRml("details-addons", rml);
}

void GameDetailsPanel::SetRml(const char * id, const std::string & rml)
{
    Rml::Element * element = m_panel->GetElementById(id);
    if (element != nullptr && element->GetInnerRML() != rml)
    {
        element->SetInnerRML(rml);
    }
}

void GameDetailsPanel::SetLocked(const char * id, bool locked, const std::string & lockedHint, const std::string & hint)
{
    if (Rml::Element * element = m_panel->GetElementById(id))
    {
        element->SetClass("locked", locked);
        const std::string & title = locked ? lockedHint : hint;
        if (element->GetAttribute<Rml::String>("title", "") != title)
        {
            element->SetAttribute("title", title);
        }
    }
}
