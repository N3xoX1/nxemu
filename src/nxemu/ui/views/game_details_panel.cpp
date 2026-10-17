#include "game_details_panel.h"
#include "rml_events.h"
#include "settings_context.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cstdio>
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

// The loader lists a game's add-on content as its numbers, separated by commas.
size_t AddOnContentCount(const GameAddOn & addOn)
{
    return static_cast<size_t>(std::count(addOn.version.begin(), addOn.version.end(), ',')) + 1;
}

std::string SizeText(uint64_t bytes)
{
    constexpr double kMegabyte = 1024.0 * 1024.0;
    const auto number = [](const char * format, double value) {
        char text[32];
        std::snprintf(text, sizeof(text), format, value);
        return std::string(text);
    };
    if (bytes >= 1024 * kMegabyte)
    {
        return Tr("%1 GB", {number("%.2f", bytes / (1024 * kMegabyte))});
    }
    if (bytes >= kMegabyte)
    {
        return Tr("%1 MB", {number("%.1f", bytes / kMegabyte)});
    }
    return Tr("%1 KB", {number("%.0f", bytes / 1024.0)});
}

// A piece of add-on content: its number among the game's, its size, and its
// title id. Its file says nothing else of it, not even a name.
std::string AddOnItemRml(const GameAddOnItem & item)
{
    char titleId[17];
    std::snprintf(titleId, sizeof(titleId), "%016llX", static_cast<unsigned long long>(item.titleId));
    // Versions count in steps of 0x10000.
    const std::string hint = Tr("Version %1, title ID %2", {std::to_string(item.version >> 16), titleId});
    return "<div class=\"addon-item\" title=\"" + Encoded(hint) + "\"><div class=\"addon-name\">" + Encoded(Tr("DLC %1", {std::to_string(item.titleId & 0x7FF)})) +
           "</div><div class=\"addon-item-id\">" + titleId + "</div><div class=\"addon-item-size\">" + SizeText(item.size) + "</div></div>";
}
} // namespace

GameDetailsPanel::GameDetailsPanel(Rml::Element * panel, std::function<void(size_t)> onToggleAddOn, std::function<void(size_t)> onChangeOverride) :
    m_panel(panel)
{
    UiEvents::Listen(m_panel->GetElementById("details-addons"), "click", [this, onToggleAddOn = std::move(onToggleAddOn), onChangeOverride = std::move(onChangeOverride)](Rml::Event & event) {
        if (Rml::Element * action = UiEvents::ClosestWithClass(event.GetTargetElement(), "override-action"))
        {
            const int index = action->GetAttribute<int>("data-override", -1);
            if (index >= 0) onChangeOverride(static_cast<size_t>(index));
        }
        else if (Rml::Element * toggle = UiEvents::ClosestWithClass(event.GetTargetElement(), "switch"))
        {
            const int index = toggle->GetAttribute<int>("data-addon", -1);
            if (index >= 0)
            {
                onToggleAddOn(static_cast<size_t>(index));
            }
        }
        else if (Rml::Element * row = UiEvents::ClosestWithClass(event.GetTargetElement(), "expandable"))
        {
            m_addOnContentOpen = !m_addOnContentOpen;
            row->SetClass("open", m_addOnContentOpen);
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

void GameDetailsPanel::ShowDetails(const GameDetails * details)
{
    const bool known = details != nullptr;
    SetLocked("details-configure", !known, Tr("Reading game information"), Tr("Settings for this game only"));
    SetLocked("details-save", !known || details->saveDirectory.empty(), known ? Tr("This game has not saved anything yet") : std::string(), Tr("Opens the folder this game saves in"));
    SetLocked("details-cache", !known || details->cacheDirectory.empty(), known ? Tr("This game has no shader cache yet") : std::string(), Tr("Opens the folder of this game's shader cache"));
    SetLocked("details-mods", !known || details->modDirectory.empty(), {}, Tr("Opens the folder this game's mods go in"));
    if (!known)
    {
        SetRml("details-addons", {});
        return;
    }

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
            std::string rowClass = "addon-row";
            std::string arrow;
            std::string items;
            if (kind == GameAddOn::Kind::AddOnContent)
            {
                const size_t pieces = addOn.items.empty() ? AddOnContentCount(addOn) : addOn.items.size();
                count += pieces;
                name = Tr("Add-on content");
                version = TrCount("%n item", "%n items", pieces);
                if (!addOn.items.empty())
                {
                    rowClass += m_addOnContentOpen ? " expandable open" : " expandable";
                    arrow = "<img class=\"addon-arrow\" src=\"images/chevron-down.png\"/>";
                    for (const GameAddOnItem & item : addOn.items)
                    {
                        items += AddOnItemRml(item);
                    }
                    items = "<div class=\"addon-items\">" + items + "</div>";
                }
            }
            else
            {
                count++;
            }
            rows += "<div class=\"" + rowClass + "\">" + arrow + "<div class=\"addon-name\">" + Encoded(name) + "</div><div class=\"addon-version\">" +
                    Encoded(version) + "</div><div class=\"switch" + (addOn.enabled ? " on" : "") + "\" data-addon=\"" + std::to_string(i) +
                    "\"><div class=\"switch-knob\"/></div></div>" + items;
        }
        if (!rows.empty())
        {
            rml += "<div class=\"details-section\"><div class=\"details-section-title\">" + Encoded(SectionTitle(kind)) + " (" +
                   std::to_string(count) + ")</div><div class=\"details-section-line\"/></div>" + rows;
        }
    }
    if (rml.empty())
    {
        rml = "<div class=\"details-note\">" + Encoded(Tr("No update, DLC or mod found for this game.")) + "</div>";
    }
    for (bool user : {false, true})
    {
        std::string rows;
        for (size_t i = 0; i < details->overrides.size(); i++)
        {
            const GameOverride & setting = details->overrides[i];
            if (setting.user != user) continue;
            const std::string action = user ?
                "<button class=\"override-action override-remove\" data-override=\"" + std::to_string(i) + "\" title=\"" + Encoded(Tr("Remove User override; inherit NXEmu or global settings")) + "\"><div>&#xD7;</div></button>" :
                "<div class=\"override-action switch" + std::string(setting.enabled ? " on" : "") + "\" data-override=\"" + std::to_string(i) +
                "\" title=\"" + Encoded(setting.enabled ? Tr("Disable NXEmu override for this game") : Tr("Enable NXEmu override for this game")) + "\"><div class=\"switch-knob\"/></div>";
            rows += "<div class=\"addon-row" + std::string(setting.effective ? "" : " override-replaced") +
                    "\"><div class=\"addon-name\">" + Encoded(GameSettingText(setting.name)) +
                    (setting.effective ? "" : "<div class=\"override-note\">" + Encoded(setting.enabled ? Tr("Replaced by User") : Tr("Disabled")) + "</div>") +
                    "</div><div class=\"addon-version\">" + Encoded(GameSettingText(setting.value)) +
                    "</div><div class=\"override-source" + (user ? " user" : "") + "\">" + Encoded(user ? Tr("User") : std::string("NXEmu")) + "</div>" + action + "</div>";
        }
        if (!rows.empty())
        {
            rml += "<div class=\"details-section\"><div class=\"details-section-title\">" +
                   Encoded(user ? Tr("User overrides") : Tr("NXEmu overrides")) + "</div><div class=\"details-section-line\"/></div>" + rows;
        }
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
