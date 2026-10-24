#include "ui_activity.h"
#include "ui_files.h"
#include "ui_host.h"
#include "ui_renderer.h"
#include "ui_thread.h"
#include "ui_util.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <set>

namespace
{
    constexpr float kCardMinWidth = 112.0f;
    constexpr float kCardGapX = 10.0f;
    constexpr float kCardGapY = 14.0f;

    const std::set<std::string> kSettingsPages = {
        "settingsPage", "overlaySettings", "systemSettings", "graphicsSettings", "themeSettings", "setup",
        "gpuDrivers", "gpuDriverAdd", "gpuDriverDownload", "gpuDriverGroup", "gpuDriverRelease", "gameFolders",
        "controllerSettings", "controllerPlayer", "controllerChoices", "settingChoices",
    };

    class Listener final : public Rml::EventListener
    {
    public:
        explicit Listener(std::function<void(Rml::Event &)> callback) :
            m_callback(std::move(callback))
        {
        }

        void ProcessEvent(Rml::Event & event) override { m_callback(event); }
        void OnDetach(Rml::Element *) override { delete this; }

    private:
        std::function<void(Rml::Event &)> m_callback;
    };

    Rml::Property Pixels(float value)
    {
        return Rml::Property(value, Rml::Unit::PX);
    }
}

UiActivity::UiActivity(Rml::Context & context, const UiHost & host, UiRenderer & renderer, Mode mode) :
    m_context(context),
    m_host(host),
    m_renderer(renderer),
    m_mode(mode)
{
    BuildSelects();
}

UiActivity::~UiActivity()
{
    m_library.Cancel();
    ClearCards();
    if (m_document != nullptr)
    {
        m_document->Close();
    }
}

bool UiActivity::Load()
{
    ApplyTheme();
    m_themeGeneration = UiThread::Instance().Files().Generation();
    m_document = m_context.LoadDocument("ui/main.rml");
    if (m_document == nullptr)
    {
        return false;
    }
    m_gamesPage = m_document->GetElementById("games-page");
    m_settingsShell = m_document->GetElementById("settings-shell");
    m_gameList = m_document->GetElementById("game-list");
    m_settingsBody = m_document->GetElementById("settings-body");
    m_settingsScroll = m_document->GetElementById("settings-scroll");
    m_fab = m_document->GetElementById("settings-fab");
    m_document->AddEventListener(Rml::EventId::Click, new Listener([this](Rml::Event & event) { OnClick(event); }));
    m_document->AddEventListener(Rml::EventId::Change, new Listener([this](Rml::Event & event) { OnChange(event); }));
    m_document->Show();

    if (m_mode == Mode::Settings)
    {
        m_navStack = {"settingsPage"};
        ShowPage("settingsPage");
    }
    else
    {
        StartApp();
    }
    ApplyControllerBrowser();
    return true;
}

void UiActivity::Update()
{
    const int generation = UiThread::Instance().Files().Generation();
    if (generation != m_themeGeneration)
    {
        m_themeGeneration = generation;
        Rml::Factory::ClearStyleSheetCache();
        m_document->ReloadStyleSheet();
        m_layoutDirty = true;
    }
    if (m_layoutDirty)
    {
        LayoutGames();
    }
}

bool UiActivity::Animating() const
{
    return m_libraryLoading || m_gpuDownload != nullptr;
}

void UiActivity::Resized()
{
    m_layoutDirty = true;
}

bool UiActivity::IsSettingsPage(const std::string & id) const
{
    return kSettingsPages.contains(id);
}

void UiActivity::Navigate(const std::string & page)
{
    m_navStack.push_back(page);
    ShowPage(page);
}

void UiActivity::GoBack()
{
    if (Top() == "setup")
    {
        if (m_setupStep > 0)
        {
            m_setupStep--;
            m_settingsSelection = 0;
            RenderCurrentSettings();
        }
        return;
    }
    if (m_navStack.size() > 1)
    {
        m_navStack.pop_back();
        ShowPage(Top());
        return;
    }
    if (m_mode == Mode::Settings)
    {
        m_host.Request("finish");
    }
}

void UiActivity::ShowPage(const std::string & page)
{
    const bool settings = IsSettingsPage(page);
    m_gamesPage->SetClass("hidden", settings);
    m_settingsShell->SetClass("hidden", !settings);
    if (settings)
    {
        const auto remembered = m_selectionByPage.find(page);
        m_settingsSelection = remembered == m_selectionByPage.end() ? 0 : remembered->second;
        m_settingsScroll->SetScrollTop(0);
        RenderCurrentSettings();
        return;
    }
    if (m_libraryDirty)
    {
        RefreshGameList();
    }
}

std::string UiActivity::SettingsTitle(const std::string & id)
{
    if (id == "settingsPage") return "Settings";
    if (id == "overlaySettings") return "On screen controller";
    if (id == "systemSettings") return "Console";
    if (id == "graphicsSettings") return "Graphics";
    if (id == "themeSettings") return "Theme";
    if (id == "setup") return SetupTitle();
    if (id == "gpuDrivers") return "GPU driver";
    if (id == "gpuDriverAdd") return "Add a driver";
    if (id == "gpuDriverDownload") return "Download drivers";
    if (id == "gpuDriverGroup")
    {
        return m_gpuCatalogReady ? m_gpuCatalog["groups"][m_gpuGroup]["name"].asString() : "Drivers";
    }
    if (id == "gpuDriverRelease")
    {
        return m_gpuCatalogReady ? m_gpuCatalog["groups"][m_gpuGroup]["releases"][m_gpuRelease]["title"].asString() : "Driver";
    }
    if (id == "gameFolders") return "Game folders";
    if (id == "controllerSettings") return "Controllers";
    if (id == "controllerPlayer") return "Controller " + std::to_string(m_controllerPlayer + 1);
    if (id == "controllerChoices") return m_choices.title;
    if (id == "settingChoices") return m_settingChoice != nullptr ? m_settingChoice->label : "Settings";
    return "Settings";
}

void UiActivity::RenderCurrentSettings()
{
    const std::string id = Top();
    if (!IsSettingsPage(id))
    {
        return;
    }
    if (Rml::Element * title = m_document->GetElementById("settings-title"))
    {
        title->SetInnerRML(UiUtil::Escape(SettingsTitle(id)));
    }
    if (Rml::Element * back = m_document->GetElementById("back-button"))
    {
        back->SetClass("invisible", id == "setup" && m_setupStep == 0);
    }

    if (id == "settingsPage")
    {
        RenderSettingsHome();
    }
    else if (id == "overlaySettings")
    {
        RenderOverlaySettings();
    }
    else if (id == "systemSettings")
    {
        RenderSystemSettings();
    }
    else if (id == "graphicsSettings")
    {
        RenderGraphicsSettings();
    }
    else if (id == "themeSettings")
    {
        RenderThemeSettings();
    }
    else if (id == "setup")
    {
        RenderSetup();
    }
    else if (id == "gpuDrivers")
    {
        RenderGpuDrivers();
    }
    else if (id == "gpuDriverAdd")
    {
        RenderGpuDriverAdd();
    }
    else if (id == "gpuDriverDownload")
    {
        RenderGpuDriverDownload();
    }
    else if (id == "gpuDriverGroup")
    {
        RenderGpuDriverGroup();
    }
    else if (id == "gpuDriverRelease")
    {
        RenderGpuDriverRelease();
    }
    else if (id == "gameFolders")
    {
        RenderGameFolders();
    }
    else if (id == "controllerSettings")
    {
        RenderControllerSettings();
    }
    else if (id == "controllerPlayer")
    {
        RenderControllerPlayer();
    }
    else if (id == "controllerChoices")
    {
        RenderControllerChoices();
    }
    else if (id == "settingChoices")
    {
        RenderSettingChoices();
    }
    RefreshSettingsSelection();
}

void UiActivity::ApplyTheme()
{
    UiThread::Instance().Files().SetDark(m_host.RequestBool("isDarkTheme"));
}

void UiActivity::OnClick(Rml::Event & event)
{
    for (Rml::Element * element = event.GetTargetElement(); element != nullptr; element = element->GetParentNode())
    {
        if (RunAction(element))
        {
            return;
        }
    }
}

void UiActivity::OnChange(Rml::Event & event)
{
    Rml::Element * target = event.GetTargetElement();
    if (m_building)
    {
        return;
    }
    const int index = target != nullptr ? target->GetAttribute<int>("data-slider", -1) : -1;
    if (index < 0 || index >= static_cast<int>(m_sliders.size()))
    {
        return;
    }
    const std::function<std::string(float)> changed = m_sliders[index];
    const std::string text = changed(event.GetParameter<float>("value", 0.0f));
    if (Rml::Element * label = m_document->GetElementById("slider-value-" + std::to_string(index)))
    {
        label->SetInnerRML(UiUtil::Escape(text));
    }
}

bool UiActivity::RunAction(Rml::Element * element)
{
    if (const int action = element->GetAttribute<int>("data-action", -1); action >= 0)
    {
        if (action < static_cast<int>(m_actions.size()))
        {
            const std::function<void()> run = m_actions[action];
            run();
        }
        return true;
    }
    if (const int game = element->GetAttribute<int>("data-game", -1); game >= 0)
    {
        if (game < static_cast<int>(m_cards.size()))
        {
            LaunchGame(m_cards[game].path);
        }
        return true;
    }
    if (element->HasAttribute("data-command"))
    {
        RunCommand(element->GetAttribute<Rml::String>("data-command", ""));
        return true;
    }
    return false;
}

void UiActivity::RunCommand(const std::string & command)
{
    if (command == "settings")
    {
        m_host.Request("openSettings");
    }
    else if (command == "back")
    {
        GoBack();
    }
    else if (command == "fab")
    {
        if (m_fabAction)
        {
            const std::function<void()> run = m_fabAction;
            run();
        }
    }
    else if (command == "addFolder")
    {
        m_host.Request("addGameDirectory");
    }
    else if (command == "manageFolders")
    {
        Navigate("gameFolders");
    }
}
void UiActivity::Confirm(const std::string & message, std::function<void()> accepted)
{
    const int id = m_nextConfirm++;
    m_confirms[id] = std::move(accepted);
    m_host.Request("confirm", {std::to_string(id), message});
}

void UiActivity::StartApp()
{
    m_navStack = {"gamesPage"};
    const bool complete = m_host.RequestBool("isSetupComplete");
    if (complete || !UiUtil::ConfiguredFolders().empty())
    {
        if (!complete)
        {
            m_host.Request("completeSetup");
        }
        ShowPage("gamesPage");
        return;
    }
    m_setupSteps = {"folders", "controls"};
    const std::string recommendation = m_host.Request("gpuDriverRecommendation");
    if (GpuDriverSupported() && GpuDriverLabel() == "System" && !recommendation.empty())
    {
        m_setupSteps.push_back("driver");
    }
    m_setupStep = 0;
    m_navStack = {"gamesPage", "setup"};
    ShowPage("setup");
}

void UiActivity::ClearCards()
{
    for (const Card & card : m_cards)
    {
        if (card.icon.starts_with("nxmem:"))
        {
            m_renderer.RemoveMemoryImage(card.icon);
        }
    }
    m_cards.clear();
}

void UiActivity::RefreshGameList()
{
    m_libraryDirty = false;
    const int gen = ++m_gameListGen;
    m_library.Cancel();
    m_libraryLoading = false;
    SyncGameSelection(-1);
    ClearCards();

    if (UiUtil::ConfiguredFolders().empty())
    {
        SetLibraryStatus("");
        RenderEmptyGames("no-folders");
        return;
    }
    m_gameList->SetInnerRML("<div class=\"empty-hint\">Scanning folders...</div>");
    SetLibraryStatus("Scanning...");
    m_scanning = true;
    m_quietScan = false;
    m_host.Request("requestGameLibraryScan", {std::to_string(gen)});
}
void UiActivity::RenderEmptyGames(const char * kind)
{
    SyncGameSelection(-1);
    const std::string_view which = kind;
    std::string rml;
    if (which == "no-folders")
    {
        rml = "<div class=\"empty-state\"><p class=\"empty-title\">Add a game folder</p>"
              "<p class=\"empty-copy\">NxEmu lists games from folders you choose. Your files stay where they are.</p>"
              "<div class=\"primary-btn\" data-command=\"addFolder\">Add game folder</div></div>";
    }
    else if (which == "no-roms")
    {
        rml = "<div class=\"empty-state\"><p class=\"empty-title\">No games found</p>"
              "<p class=\"empty-copy\">No .nro, .dxci, or .dnsp files were found in your folders.</p>"
              "<div class=\"primary-btn\" data-command=\"manageFolders\">Manage folders</div></div>";
    }
    else
    {
        rml = "<div class=\"empty-state\"><p class=\"empty-title\">Couldn't read these files</p>"
              "<p class=\"empty-copy\">Files were found, but none could be opened as games. Check that they are valid dumps.</p>"
              "<div class=\"link-btn\" data-command=\"refresh\">Try again</div></div>";
    }
    m_gameList->SetInnerRML(rml);
}

void UiActivity::SetLibraryStatus(const std::string & text)
{
    if (Rml::Element * status = m_document->GetElementById("library-status"))
    {
        status->SetInnerRML(UiUtil::Escape(text));
        status->SetClass("hidden", text.empty());
    }
}

void UiActivity::LayoutGames()
{
    const float dp = m_context.GetDensityIndependentPixelRatio();
    const float width = static_cast<float>(m_context.GetDimensions().x - m_insets[0] - m_insets[2]) - 32.0f * dp;
    if (width <= 0)
    {
        return;
    }
    m_layoutDirty = false;
    const float gapX = kCardGapX * dp;
    m_columns = std::max(1, static_cast<int>((width + gapX) / (kCardMinWidth * dp + gapX)));
    const float cardWidth = std::floor((width - gapX * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns));
    int position = 0;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        if (m_cards[i].failed)
        {
            continue;
        }
        Rml::Element * card = m_document->GetElementById("game-" + std::to_string(i));
        if (card == nullptr)
        {
            continue;
        }
        const bool lastColumn = position % m_columns == m_columns - 1;
        card->SetProperty(Rml::PropertyId::Width, Pixels(cardWidth));
        card->SetProperty(Rml::PropertyId::MarginRight, Pixels(lastColumn ? 0.0f : gapX));
        card->SetProperty(Rml::PropertyId::MarginBottom, Pixels(kCardGapY * dp));
        if (Rml::Element * art = card->GetFirstChild())
        {
            art->SetProperty(Rml::PropertyId::Width, Pixels(cardWidth));
            art->SetProperty(Rml::PropertyId::Height, Pixels(cardWidth));
        }
        position++;
    }
}

std::vector<size_t> UiActivity::SelectableGames() const
{
    std::vector<size_t> cards;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        if (!m_cards[i].loading && !m_cards[i].failed)
        {
            cards.push_back(i);
        }
    }
    return cards;
}

void UiActivity::SyncGameSelection(int index)
{
    const std::vector<size_t> cards = SelectableGames();
    if (index < 0 || cards.empty() || !ControllerAttached())
    {
        m_selectedGame = -1;
    }
    else
    {
        m_selectedGame = std::min(index, static_cast<int>(cards.size()) - 1);
    }
    for (size_t i = 0; i < cards.size(); i++)
    {
        if (Rml::Element * card = m_document->GetElementById("game-" + std::to_string(cards[i])))
        {
            const bool selected = static_cast<int>(i) == m_selectedGame;
            card->SetClass("is-selected", selected);
            if (selected)
            {
                card->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            }
        }
    }
    UpdatePlayHint();
}

void UiActivity::LaunchGame(const std::string & path)
{
    m_host.Request("launchGame", {path});
}

bool UiActivity::ControllerAttached() const
{
    return !UiUtil::StringArray(UiUtil::ParseJson(m_host.Request("connectedControllerNames"))).empty();
}

void UiActivity::ApplyControllerBrowser()
{
    const bool attached = ControllerAttached();
    if (Rml::Element * hint = m_document->GetElementById("settings-hint"))
    {
        hint->SetClass("hidden", !attached);
    }
    if (Rml::Element * hint = m_document->GetElementById("touch-hint"))
    {
        hint->SetClass("hidden", attached);
    }
    if (Rml::Element * hint = m_document->GetElementById("back-hint"))
    {
        hint->SetClass("hidden", !attached);
    }
    if (!attached)
    {
        SyncGameSelection(-1);
        SyncSettingsSelection(-1);
        return;
    }
    if (IsSettingsPage(Top()))
    {
        const auto remembered = m_selectionByPage.find(Top());
        const int index = m_settingsSelection >= 0 ? m_settingsSelection : remembered == m_selectionByPage.end() ? 0 : remembered->second;
        SyncSettingsSelection(index);
    }
    if (m_selectedGame < 0 && !SelectableGames().empty())
    {
        SyncGameSelection(0);
    }
    else
    {
        UpdatePlayHint();
    }
}

void UiActivity::UpdatePlayHint()
{
    if (Rml::Element * hint = m_document->GetElementById("play-hint"))
    {
        hint->SetClass("hidden", m_selectedGame < 0 || !ControllerAttached());
    }
}

std::vector<Rml::Element *> UiActivity::SettingsRows() const
{
    std::vector<Rml::Element *> rows;
    if (!IsSettingsPage(Top()))
    {
        return rows;
    }
    Rml::ElementList found;
    m_settingsBody->QuerySelectorAll(found, ".nav");
    rows.assign(found.begin(), found.end());
    if (m_fabAction)
    {
        rows.push_back(m_fab);
    }
    return rows;
}

void UiActivity::SyncSettingsSelection(int index)
{
    const std::vector<Rml::Element *> rows = SettingsRows();
    for (Rml::Element * row : rows)
    {
        row->SetClass("is-selected", false);
    }
    if (index < 0 || rows.empty() || !ControllerAttached())
    {
        m_settingsSelection = -1;
        return;
    }
    m_settingsSelection = std::min(index, static_cast<int>(rows.size()) - 1);
    m_selectionByPage[Top()] = m_settingsSelection;
    rows[m_settingsSelection]->SetClass("is-selected", true);
    rows[m_settingsSelection]->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
}

void UiActivity::RefreshSettingsSelection()
{
    SyncSettingsSelection(m_settingsSelection < 0 ? 0 : m_settingsSelection);
}
