#include "settings_context.h"
#include <nxemu-core/settings/settings.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/ui_preferences.h>

std::string GameSettingText(const std::string & text)
{
    constexpr struct { const char * core; const char * ui; } names[] = {
        {"Docked", TrMark("TV")},
        {"VSync", TrMark("Vertical sync")},
        {"Console region", TrMark("Region")},
        {"Speed limit", TrMark("Limit speed")},
        {"Audio mode", TrMark("Output mode")},
        {"GPU accuracy", TrMark("Accuracy level")},
        {"Disk shader cache", TrMark("Keep shaders on the disk")},
        {"Background shaders", TrMark("Build shaders in the background")},
        {"Graphics thread", TrMark("Graphics on their own thread")},
        {"Presentation thread", TrMark("Presentation on its own thread")},
        {"Maximum GPU clocks", TrMark("Keep the graphics device busy")},
        {"CPU / system synchronization", TrMark("CPU and system in step")},
        {"Stretch", TrMark("Stretch to the window")},
        {"Mailbox", TrMark("On, lowest delay")},
        {"Relaxed", TrMark("On, late pictures tear")},
        {"Nearest", TrMark("Nearest neighbour")},
        {"As requested", TrMark("As the game asks")},
        {"As accuracy", TrMark("As the accuracy level")},
        {"CPU", TrMark("Processor")},
        {"GPU", TrMark("Graphics device")},
        {"CPU asynchronous", TrMark("Processor, in the background")},
        {"Follow language", TrMark("Follows the console language.")},
        // Named by no document: marked here for the overrides a game lists.
        {"Volume", TrMark("Volume")},
        {"Sharpness", TrMark("Sharpness")},
    };
    for (const auto & name : names)
    {
        if (text == name.core) return Tr(name.ui);
    }
    return Tr(text.c_str());
}

void SettingsContext::Global()
{
    m_programId = 0;
    m_title.clear();
    m_error.clear();
}

void SettingsContext::Game(uint64_t programId, std::string title)
{
    m_programId = programId;
    m_title = std::move(title);
    m_error.clear();
}

int32_t SettingsContext::GetInt(const char * key) const
{
    SettingsStore::GlobalReadScope global;
    return IsGame() && GameSettings::Definition(key) != nullptr ? static_cast<int32_t>(GameSettings::GetInstance().Resolve(m_programId, key).asInt64()) :
           SettingsStore::GetInstance().GetInt(key);
}

bool SettingsContext::GetBool(const char * key) const
{
    SettingsStore::GlobalReadScope global;
    return IsGame() && GameSettings::Definition(key) != nullptr ? GameSettings::GetInstance().Resolve(m_programId, key).asBool() :
           SettingsStore::GetInstance().GetBool(key);
}

const char * SettingsContext::GetString(const char * key) const
{
    return SettingsStore::GetInstance().GetString(key);
}

int32_t SettingsContext::GetDefaultInt(const char * key) const { return SettingsStore::GetInstance().GetDefaultInt(key); }
bool SettingsContext::GetDefaultBool(const char * key) const { return SettingsStore::GetInstance().GetDefaultBool(key); }
const char * SettingsContext::GetDefaultString(const char * key) const { return SettingsStore::GetInstance().GetDefaultString(key); }

void SettingsContext::SetInt(const char * key, int32_t value)
{
    if (IsGame())
    {
        m_changed = true;
        m_error.clear();
        GameSettings::GetInstance().SetUser(m_programId, key, JsonValue(value), m_error);
    }
    else SettingsStore::GetInstance().SetInt(key, value);
}

void SettingsContext::SetBool(const char * key, bool value)
{
    if (IsGame())
    {
        m_changed = true;
        m_error.clear();
        GameSettings::GetInstance().SetUser(m_programId, key, JsonValue(value), m_error);
    }
    else SettingsStore::GetInstance().SetBool(key, value);
}

void SettingsContext::SetString(const char * key, const char * value)
{
    if (!IsGame()) SettingsStore::GetInstance().SetString(key, value);
}

bool SettingsContext::InheritCommand(const std::string & command)
{
    if (!IsGame() || command.rfind("inherit:", 0) != 0) return false;
    m_error.clear();
    m_changed = true;
    const std::string key = command.substr(8);
    GameSettings::GetInstance().Inherit(m_programId, key.c_str(), m_error);
    return true;
}

void SettingsContext::InheritAll()
{
    if (IsGame())
    {
        m_changed = true;
        m_error.clear();
        GameSettings::GetInstance().Inherit(m_programId, nullptr, m_error);
    }
}

void SettingsContext::Decorate(Rml::ElementDocument * document) const
{
    document->SetClass("per-game", IsGame());
    if (auto * banner = document->GetElementById("settings-scope"))
    {
        // The settings of all games need no word; those of one game say whose
        // they are, and what is wrong or out of their hands is always said.
        std::string rml;
        if (IsGame())
        {
            rml += "<div class=\"scope-title\">" + Rml::StringUtilities::EncodeRml(Tr("Settings for %1", {m_title})) + "<span class=\"scope-id\">" +
                   Rml::StringUtilities::EncodeRml(GameSettings::TitleId(m_programId)) + "</span></div>";
            if (m_error.empty())
            {
                rml += "<div>" + Rml::StringUtilities::EncodeRml(Tr("Global < NXEmu < User. Editing a setting creates a User override; Inherit removes it. Changes apply on the next launch.")) + "</div>";
            }
        }
        if (!m_error.empty())
        {
            rml += "<div>" + Rml::StringUtilities::EncodeRml(m_error) + "</div>";
        }
        if (UiPreferences::HasStatusOverride())
        {
            rml += "<div class=\"scope-controls\">" + Rml::StringUtilities::EncodeRml(Tr("Choices in the status bar take priority over these settings.")) + "</div>";
        }
        banner->SetInnerRML(rml);
        banner->SetClass("save-error", !m_error.empty());
        banner->SetClass("of-game", IsGame());
        banner->SetClass("empty", rml.empty());
    }
    if (!IsGame()) return;
    Rml::ElementList globalOnly;
    document->GetElementsByClassName(globalOnly, "global-only");
    for (auto * element : globalOnly) element->SetProperty("display", "none");
    Rml::ElementList rows;
    document->GetElementsByClassName(rows, "row");
    for (auto * row : rows) row->SetClass("setting-overridden", false);
    for (const auto & definition : GameSettings::Definitions())
    {
        auto * control = document->GetElementById(definition.control);
        if (control == nullptr) continue;
        auto * row = control;
        while (row != nullptr && !row->IsClassSet("row")) row = row->GetParentNode();
        if (row == nullptr) continue;
        const std::string id = std::string("source:") + definition.key;
        auto * status = document->GetElementById(id);
        if (status == nullptr)
        {
            Rml::ElementList origins;
            row->GetElementsByClassName(origins, "setting-origins");
            Rml::Element * footer = origins.empty() ? nullptr : origins.front();
            if (footer == nullptr)
            {
                auto element = document->CreateElement("div");
                element->SetClass("setting-origins", true);
                footer = row->AppendChild(std::move(element));
            }
            auto element = document->CreateElement("div");
            element->SetId(id);
            element->SetClass("setting-origin", true);
            status = footer->AppendChild(std::move(element));
        }
        const auto source = GameSettings::GetInstance().Origin(m_programId, definition.key);
        const std::string label = source == GameSettings::Source::User ? Tr("User override") : source == GameSettings::Source::NXEmu ? Tr("NXEmu override") : std::string();
        const bool user = source == GameSettings::Source::User;
        const bool overridden = source != GameSettings::Source::Global;
        if (overridden) row->SetClass("setting-overridden", true);
        status->SetClass("source-global", !overridden);
        status->SetClass("source-user", user);
        status->SetClass("source-nxemu", source == GameSettings::Source::NXEmu);
        if (!overridden)
        {
            status->SetInnerRML("");
            continue;
        }
        // The row of the speed limit holds two settings: each origin names its own.
        const std::string controlId = definition.control;
        const std::string property = controlId == "limit" || controlId == "speed-range" ?
            "<div class=\"origin-property\">" + Rml::StringUtilities::EncodeRml(GameSettingText(definition.label)) + "</div>" : "";
        status->SetInnerRML(property + "<span class=\"origin-label\">" + Rml::StringUtilities::EncodeRml(label) + "</span>" +
                           (user ? "<button class=\"inherit-setting\" data-command=\"inherit:" + std::string(definition.key) + "\"><div>" + Rml::StringUtilities::EncodeRml(Tr("Inherit")) + "</div></button>" : ""));
    }
}
