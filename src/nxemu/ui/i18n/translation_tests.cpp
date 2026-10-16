#include "translation.h"
#include <nxemu/ui/host/system_fonts.h>
#include <nxemu/ui/library/ui_preferences.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-os/os_types.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <RmlUi/Core.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const std::string & message)
{
    if (!condition) throw std::runtime_error(message);
}

// Document parsing and text layout need a renderer, but no GPU.
class TestRenderer final : public Rml::RenderInterface
{
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i &, const Rml::String &) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

struct TemporaryDocument
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("nxemu-translation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".rml");
    ~TemporaryDocument() { std::error_code error; std::filesystem::remove(path, error); }
};
} // namespace

int main(int argc, char ** argv)
{
    Rml::SystemInterface system;
    TestRenderer renderer;
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&renderer);
    bool initialised = false;
    try
    {
        Check(argc == 2, "Expected the UI source directory");
        const std::filesystem::path ui = argv[1];
        Check(Rml::Initialise(), "RmlUi initialisation failed");
        initialised = true;
        for (const char * font : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf"})
        {
            Check(Rml::LoadFontFace((ui / "assets" / "fonts" / font).string()), "Latin font failed to load");
        }
        const auto systemFonts = SystemFallbackFonts();
        for (const SystemFont & font : systemFonts)
        {
            Check(Rml::LoadFontFace(font.path.string(), true, Rml::Style::FontWeight::Normal, font.faceIndex), "Installed fallback font failed to load: " + font.path.string());
            std::cout << "System font: " << font.path << " (face " << font.faceIndex << ")\n";
        }
#if defined(__APPLE__)
        Check(systemFonts.size() >= 3, "macOS should provide Cyrillic and Asian fallback fonts");
#endif
        Rml::Context * context = Rml::CreateContext("translations", {1280, 720});
        Check(context != nullptr, "Could not create a document context");
        TemporaryDocument test;
        std::ofstream(test.path) << R"(<rml><head><style>body { font-family: LatoLatin; }</style></head><body>
            <div id="text">  Settings  </div><input id="field" placeholder="Search games..." title="Settings"/>
            <div id="untouched" translate="no">Settings</div><div id="dynamic"></div>
            </body></rml>)";
        const char * const settings[] = {"Einstellungen", "Settings", "Ajustes", "Paramètres", "Impostazioni",
                                         "Instellingen", "Configurações", "Настройки", "設定", "설정", "设置"};
        size_t index = 0;
        for (const auto & language : Translation::Languages)
        {
            const std::string code = language.code;
            Check(Translation::IsSupportedLanguage(code), "Language registry rejected " + code);
            if (code == "en") Translation::Clear();
            else Check(Translation::Load(ui / "lang" / (code + ".po")), "Could not load " + code);
            Check(Tr("Settings") == settings[index++], "Settings translation is incorrect for " + code);
            Check(Tr("Text absent from every catalogue") == "Text absent from every catalogue", "Missing text fallback failed");
            const std::string player = Tr("Player %1", {"42"});
            Check(player.find("42") != std::string::npos && player.find("%1") == std::string::npos, "Number substitution failed: " + code);
            for (uint64_t count : {0, 1, 2, 5, 11, 21, 22, 25, 101})
            {
                const auto games = TrCount("%n game", "%n games", count);
                Check(games.find(std::to_string(count)) != std::string::npos && games.find("%n") == std::string::npos, "Count substitution failed: " + code);
            }
            Rml::ElementDocument * document = Translation::LoadDocument(*context, test.path);
            Check(document != nullptr, "Document failed to load: " + code);
            Check(document->GetElementById("text")->GetInnerRML().find(Tr("Settings")) != std::string::npos, "Document text is untranslated: " + code);
            auto * field = document->GetElementById("field");
            Check(field->GetAttribute<Rml::String>("placeholder", "") == Tr("Search games..."), "Placeholder is untranslated: " + code);
            Check(field->GetAttribute<Rml::String>("title", "") == Tr("Settings"), "Tooltip is untranslated: " + code);
            Check(document->GetElementById("untouched")->GetInnerRML() == "Settings", "Explicitly excluded text was translated");
            document->Show();
            context->Update();
            document->Close();
            context->Update();
        }
        Check(!Translation::IsSupportedLanguage("../fr"), "Unsupported catalogue path was accepted");
        const char * const consoleLanguages[] = {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh", "en", "fr", "es", "zh", "zh", "pt"};
        for (int language = 0; language < 18; ++language)
        {
            Check(Translation::ResolveLanguage(language, "") == consoleLanguages[language], "Automatic console language mapping failed");
            Check(Translation::ResolveLanguage(language, "de") == "de", "Manual override did not take priority");
            Check(Translation::ResolveLanguage(language, "invalid") == consoleLanguages[language], "Invalid override did not fall back to automatic");
        }
        Check(Translation::ResolveLanguage(-1, "") == "en" && Translation::ResolveLanguage(100, "") == "en", "Unknown console language fallback failed");

        // Exercise the persisted preference against the real global/game layers.
        TemporaryDocument config;
        auto & store = SettingsStore::GetInstance();
        Check(store.Initialize(config.path.string().c_str(), config.path.parent_path().string().c_str()), "Temporary settings failed");
        store.SetInt(NXOsSetting::LanguageIndex, static_cast<int32_t>(Language::FrenchCanadian));
        Check(UiPreferences::InterfaceLanguageChoice().empty() && UiPreferences::InterfaceLanguageCode() == "fr", "New preference should follow the console");
        JsonValue overrides;
        overrides[NXOsSetting::LanguageIndex] = static_cast<int32_t>(Language::Japanese);
        store.SetGameOverrides(overrides);
        Check(store.GetInt(NXOsSetting::LanguageIndex) == static_cast<int32_t>(Language::Japanese), "Game fixture did not override console language");
        Check(UiPreferences::InterfaceLanguageCode() == "fr", "Per-game language should not change interface language");
        UiPreferences::SetString(UiPreferences::InterfaceLanguage, "es");
        Check(UiPreferences::InterfaceLanguageCode() == "es", "Explicit preference should override the console");
        store.SetInt(NXOsSetting::LanguageIndex, static_cast<int32_t>(Language::ChineseTraditional));
        Check(UiPreferences::InterfaceLanguageCode() == "es", "Console change should preserve interface override");
        Check(store.Initialize(config.path.string().c_str(), config.path.parent_path().string().c_str()), "Saved settings failed to reload");
        Check(UiPreferences::InterfaceLanguageChoice() == "es", "Override should survive restart");
        UiPreferences::SetString(UiPreferences::InterfaceLanguage, "");
        Check(UiPreferences::InterfaceLanguageCode() == "zh", "Disabling override should follow global console again");
        Check(store.Initialize(config.path.string().c_str(), config.path.parent_path().string().c_str()), "Automatic settings failed to reload");
        Check(UiPreferences::InterfaceLanguageChoice().empty(), "Automatic preference should survive restart");
        // Status bar choices beat the launch snapshot without changing globals.
        store.SetInt(NXVideoSetting::ScalingFilter, 1);
        store.SetInt(NXOsSetting::AudioVolume, 100);
        store.SetBool(NXOsSetting::AudioMuted, false);
        overrides = JsonValue();
        overrides[NXVideoSetting::ScalingFilter] = 2;
        overrides[NXOsSetting::AudioVolume] = 65;
        overrides[NXOsSetting::AudioMuted] = true;
        store.SetGameOverrides(overrides);
        UiPreferences::SetStatusInt(NXVideoSetting::ScalingFilter, 5);
        UiPreferences::SetStatusInt(NXOsSetting::AudioVolume, 120);
        UiPreferences::SetStatusBool(NXOsSetting::AudioMuted, false);
        Check(store.GetInt(NXVideoSetting::ScalingFilter) == 5 && store.GetInt(NXOsSetting::AudioVolume) == 120 &&
              !store.GetBool(NXOsSetting::AudioMuted), "Status choices must beat game int and bool overrides");
        {
            SettingsStore::GlobalReadScope global;
            Check(store.GetInt(NXVideoSetting::ScalingFilter) == 1 && store.GetInt(NXOsSetting::AudioVolume) == 100 &&
                  !store.GetBool(NXOsSetting::AudioMuted), "Saving must preserve original global values");
        }
        UiPreferences::SetStatusInt(NXVideoSetting::ScalingFilter, 999);
        Check(store.GetInt(NXVideoSetting::ScalingFilter) == 5, "Invalid persisted filter was accepted");
        Check(store.Initialize(config.path.string().c_str(), config.path.parent_path().string().c_str()), "Status settings failed to reload");
        UiPreferences::ApplyStatusOverrides();
        Check(store.GetInt(NXVideoSetting::ScalingFilter) == 5 && store.GetInt(NXOsSetting::AudioVolume) == 120 &&
              !store.GetBool(NXOsSetting::AudioMuted), "Explicit choices must survive restart");
        UiPreferences::ResetStatusOverride(NXVideoSetting::ScalingFilter);
        UiPreferences::ResetStatusOverride(NXOsSetting::AudioVolume);
        UiPreferences::ResetStatusOverride(NXOsSetting::AudioMuted);
        Check(store.GetInt(NXVideoSetting::ScalingFilter) == 2 && store.GetInt(NXOsSetting::AudioVolume) == 65 &&
              store.GetBool(NXOsSetting::AudioMuted), "Reset must restore the game's launch snapshot");
        Check(store.Initialize(config.path.string().c_str(), config.path.parent_path().string().c_str()), "Reset choices failed to reload");
        UiPreferences::ApplyStatusOverrides();
        Check(!store.HasControlOverride(NXVideoSetting::ScalingFilter) && !store.HasControlOverride(NXOsSetting::AudioMuted),
              "Reset must persist");
        UiPreferences::SetStatusInt(NXVideoSetting::ScalingFilter, 2);
        UiPreferences::SetStatusBool(NXOsSetting::AudioMuted, true);
        Check(!store.HasControlOverride(NXVideoSetting::ScalingFilter) && !store.HasControlOverride(NXOsSetting::AudioMuted),
              "Choosing what the game says must follow the game again");
        store.SetGameOverrides(JsonValue());
        Check(store.GetInt(NXVideoSetting::ScalingFilter) == 1 && store.GetInt(NXOsSetting::AudioVolume) == 100,
              "Reset without a game must follow the original globals");
        SettingsStore::CleanUp();

        Translation::Clear();
        Rml::ElementDocument * live = Translation::LoadDocument(*context, test.path);
        Check(live != nullptr, "Live document failed to load");
        live->GetElementById("dynamic")->SetInnerRML("Settings");
        live->GetElementById("field")->SetAttribute("value", "My game Settings");
        for (const char * code : {"fr", "de", "zh", "en", "fr", "en"})
        {
            Check(Translation::UseLanguage(code, ui / "missing", ui / "lang"), "Catalogue selection with source fallback failed");
            Translation::RefreshDocument(*live);
            Check(live->GetElementById("text")->GetInnerRML().find(Tr("Settings")) != std::string::npos, "Live text switching failed");
            Check(live->GetElementById("field")->GetAttribute<Rml::String>("title", "") == Tr("Settings"), "Live tooltip switching failed");
            Check(live->GetElementById("field")->GetAttribute<Rml::String>("placeholder", "") == Tr("Search games..."), "Live placeholder switching failed");
            Check(live->GetElementById("field")->GetAttribute<Rml::String>("value", "") == "My game Settings", "Switching changed user input");
            Check(live->GetElementById("dynamic")->GetInnerRML() == "Settings", "Switching translated dynamic user content");
            Check(live->GetElementById("untouched")->GetInnerRML() == "Settings", "Switching translated excluded text");
        }
        live->Close();
        context->Update();
        Check(Translation::UseLanguage("fr", ui / "lang", ui / "missing"), "Primary catalogue directory failed");
        Check(!Translation::UseLanguage("de", ui / "missing", ui / "missing") && Tr("Settings") == "Settings", "Missing catalogue should fall back to English");
        Check(Translation::Load(ui / "lang/fr.po"), "French catalogue failed");
        Check(TrCount("%n game", "%n games", 0) == "0 jeu", "French zero plural failed");
        Check(TrCount("%n game", "%n games", 2) == "2 jeux", "French plural failed");
        Check(!Translation::Load(ui / "lang/missing.po") && Tr("Settings") == "Paramètres", "Failed load changed the active language");
        Check(Translation::Load(ui / "lang/ru.po"), "Russian catalogue failed");
        Check(TrCount("%n game", "%n games", 1) == "1 игра", "Russian singular failed");
        Check(TrCount("%n game", "%n games", 2) == "2 игры", "Russian few form failed");
        Check(TrCount("%n game", "%n games", 11) == "11 игр", "Russian many form failed");
        Check(TrCount("%n game", "%n games", 21) == "21 игра", "Russian 21 form failed");
        Translation::Clear();
        Check(Tr("Settings") == "Settings", "English reset failed");
        Rml::Shutdown();
        initialised = false;
        std::cout << "All 11 languages: console mapping, override, live switching, catalogues, plurals, documents and fonts passed\n";
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << error.what() << '\n';
        if (initialised) Rml::Shutdown();
        return 1;
    }
}
