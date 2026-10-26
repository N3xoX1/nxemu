#include "ui_activity.h"
#include "settings/ui_identifiers.h"
#include "ui_files.h"
#include "ui_host.h"
#include "ui_renderer.h"
#include "ui_thread.h"
#include "ui_util.h"

#include <nxemu-core/settings/identifiers.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace
{
    constexpr float kCardMinWidths[] = {84.0f, 112.0f, 160.0f};
    constexpr float kListIconSize = 56.0f;
    constexpr float kCardGapX = 10.0f;
    constexpr float kCardGapY = 14.0f;

    const std::set<std::string> kSettingsPages = {
        "hubPage", "about", "interfaceSettings", "gameListSettings", "gameProperties", "gameInfo", "gameAddOns", "gameSaveData", "settingsPage", "overlaySettings", "systemSettings", "graphicsSettings", "themeSettings", "setup",
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
    m_gameList->SetClass("list", UiUtil::ReadInt(NXUISetting::GameListView) == 1);
    m_document->AddEventListener(Rml::EventId::Click, new Listener([this](Rml::Event & event) { OnClick(event); }));
    m_document->AddEventListener(Rml::EventId::Change, new Listener([this](Rml::Event & event) { OnChange(event); }));
    m_document->Show();

    if (m_mode != Mode::Library)
    {
        const char * home = m_mode == Mode::Hub ? "hubPage" : "settingsPage";
        m_navStack = {home};
        ShowPage(home);
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

void UiActivity::SetInsets(int left, int top, int right, int bottom)
{
    m_insets[0] = left;
    m_insets[1] = top;
    m_insets[2] = right;
    m_insets[3] = bottom;
    if (m_document != nullptr)
    {
        m_document->SetProperty(Rml::PropertyId::PaddingLeft, Pixels(static_cast<float>(left)));
        m_document->SetProperty(Rml::PropertyId::PaddingTop, Pixels(static_cast<float>(top)));
        m_document->SetProperty(Rml::PropertyId::PaddingRight, Pixels(static_cast<float>(right)));
        m_document->SetProperty(Rml::PropertyId::PaddingBottom, Pixels(static_cast<float>(bottom)));
    }
    m_layoutDirty = true;
}

void UiActivity::Event(const std::string & name, const std::string & payload)
{
    if (name == "setting")
    {
        OnSettingChanged(payload);
    }
    else if (name == "libraryPaths")
    {
        const JsonValue result = UiUtil::ParseJson(payload);
        if (static_cast<int>(result["gen"].asInt64()) == m_gameListGen)
        {
            OnGameLibraryPaths(result["paths"]);
        }
    }
    else if (name == "gpuCatalog")
    {
        OnGpuDriverCatalog(UiUtil::ParseJson(payload));
    }
    else if (name == "gpuDownload")
    {
        OnGpuDriverDownload(UiUtil::ParseJson(payload));
    }
    else if (name == "gpuDrivers")
    {
        RenderGpuDriversPage();
    }
    else if (name == "firmware")
    {
        if (Top() == "hubPage")
        {
            RenderCurrentSettings();
        }
    }
    else if (name == "controllers")
    {
        RenderControllersSummary();
    }
    else if (name == "controllerPages")
    {
        RenderControllerPages();
    }
    else if (name == "confirm")
    {
        const JsonValue result = UiUtil::ParseJson(payload);
        const auto itr = m_confirms.find(static_cast<int>(result["id"].asInt64()));
        if (itr != m_confirms.end())
        {
            std::function<void()> accepted = std::move(itr->second);
            m_confirms.erase(itr);
            if (result["ok"].asBool())
            {
                accepted();
            }
        }
    }
    else if (name == "resume")
    {
        const int sort = UiUtil::ReadInt(NXUISetting::GameListSort);
        if ((sort == 2 || sort == 4) && !m_libraryLoading && !m_cards.empty())
        {
            SortCards();
        }
        ApplyGameListOptions();
        ApplyTheme();
        RenderControllersSummary();
        RescanGameList();
    }
    else if (name == "pad")
    {
        OnPad(payload);
    }
    else if (name == "searchText")
    {
        if (m_searching)
        {
            m_search = payload;
            ShowSearchText();
            ApplySearch();
        }
    }
    else if (name == "gameDataChanged")
    {
        ReloadGameProperties();
    }
    else if (name == "longPress")
    {
        float x = 0.0f, y = 0.0f;
        if (sscanf(payload.c_str(), "%f %f", &x, &y) == 2)
        {
            OpenGamePropertiesAt(x, y);
        }
    }
    else if (name == "back")
    {
        if (!HandleBack())
        {
            m_host.Request("finish");
        }
    }
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
    if (m_mode != Mode::Library)
    {
        m_host.Request("finish");
    }
}

bool UiActivity::HandleBack()
{
    if (m_sortMenuOpen)
    {
        CloseSortMenu();
        return true;
    }
    if (m_searching && Top() == "gamesPage")
    {
        CloseSearch();
        return true;
    }
    if (Top() == "setup" && m_setupStep == 0)
    {
        return false;
    }
    if (m_navStack.size() > 1)
    {
        GoBack();
        return true;
    }
    return false;
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
    if (id == "hubPage") return "NxEmu";
    if (id == "about") return "About";
    if (id == "settingsPage") return "Settings";
    if (id == "overlaySettings") return "On screen controller";
    if (id == "systemSettings") return "Console";
    if (id == "graphicsSettings") return "Graphics";
    if (id == "interfaceSettings") return "Interface";
    if (id == "gameListSettings") return "Game list";
    if (id == "gameProperties") return m_propertiesGame.title;
    if (id == "gameInfo") return "Info";
    if (id == "gameAddOns") return "Add-ons";
    if (id == "gameSaveData") return "Save data";
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

    if (id == "hubPage")
    {
        RenderHub();
    }
    else if (id == "about")
    {
        RenderAbout();
    }
    else if (id == "settingsPage")
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
    else if (id == "interfaceSettings")
    {
        RenderInterfaceSettings();
    }
    else if (id == "gameListSettings")
    {
        RenderGameListSettings();
    }
    else if (id == "gameProperties")
    {
        RenderGameProperties();
    }
    else if (id == "gameInfo")
    {
        RenderGameInfo();
    }
    else if (id == "gameAddOns")
    {
        RenderGameAddOns();
    }
    else if (id == "gameSaveData")
    {
        RenderGameSaveData();
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
            LaunchGame(m_cards[game]);
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
    if (command == "refresh")
    {
        RefreshGameList();
    }
    else if (command == "settings")
    {
        CloseSortMenu();
        m_host.Request("openSettings");
    }
    else if (command == "search")
    {
        OpenSearch();
    }
    else if (command == "editSearch")
    {
        m_host.Request("beginTextInput", {m_search});
    }
    else if (command == "clearSearch")
    {
        m_search.clear();
        ShowSearchText();
        ApplySearch();
        m_host.Request("beginTextInput", {m_search});
    }
    else if (command == "closeSearch")
    {
        CloseSearch();
    }
    else if (command == "sort")
    {
        ToggleSortMenu();
    }
    else if (command == "closeSort")
    {
        CloseSortMenu();
    }
    else if (command.starts_with("view-"))
    {
        UiUtil::SetInt(NXUISetting::GameListView, std::atoi(command.c_str() + 5));
        UiUtil::SaveSettings();
        CloseSortMenu();
        ApplyGameListOptions();
    }
    else if (command.starts_with("sort-"))
    {
        UiUtil::SetInt(NXUISetting::GameListSort, std::atoi(command.c_str() + 5));
        UiUtil::SaveSettings();
        CloseSortMenu();
        SortCards();
        RenderGameList();
        ApplySearch();
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

void UiActivity::OnPad(const std::string & button)
{
    if (button == "y")
    {
        if (Top() == "gamesPage")
        {
            m_host.Request("openSettings");
        }
    }
    else if (button == "b")
    {
        if (ControllerAttached() && Top() != "gamesPage")
        {
            GoBack();
        }
    }
    else if (button == "a")
    {
        LaunchSelectedGame();
    }
    else if (button == "x")
    {
        const std::vector<size_t> cards = SelectableGames();
        if (Top() == "gamesPage" && m_selectedGame >= 0 && m_selectedGame < static_cast<int>(cards.size()))
        {
            OpenGameProperties(cards[m_selectedGame]);
        }
    }
    else
    {
        MoveGameSelection(button);
    }
}

void UiActivity::OnSettingChanged(const std::string & setting)
{
    if (setting == NXUISetting::GameDirectories)
    {
        m_libraryDirty = true;
        if (Top() == "gameFolders" || Top() == "settingsPage" || Top() == "setup")
        {
            RenderCurrentSettings();
        }
        if (Top() == "gamesPage")
        {
            RefreshGameList();
        }
    }
    else if (setting == NXUISetting::ThemeMode)
    {
        ApplyTheme();
        if (Top() == "themeSettings" || Top() == "interfaceSettings")
        {
            RenderCurrentSettings();
        }
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

void UiActivity::RescanGameList()
{
    if (Top() != "gamesPage" || m_scanning || m_libraryLoading)
    {
        return;
    }
    if (m_cards.empty())
    {
        RefreshGameList();
        return;
    }
    const int gen = ++m_gameListGen;
    m_scanning = true;
    m_quietScan = true;
    m_host.Request("requestGameLibraryScan", {std::to_string(gen)});
}

void UiActivity::OnGameLibraryPaths(const JsonValue & paths)
{
    std::vector<UiLibrary::File> files;
    std::vector<std::string> scan;
    for (uint32_t i = 0; i < paths.size(); i++)
    {
        const JsonValue & entry = paths[i];
        UiLibrary::File file{entry["path"].asString(), static_cast<uint64_t>(entry["size"].asInt64()), entry["modified"].asInt64()};
        if (file.path.empty())
        {
            continue;
        }
        scan.push_back(file.path + "|" + std::to_string(file.size) + "|" + std::to_string(file.modified));
        m_fileSizes[file.path] = file.size;
        m_fileTimes[file.path] = file.modified;
        files.push_back(std::move(file));
    }
    std::sort(scan.begin(), scan.end());
    m_scanning = false;
    if (m_quietScan)
    {
        m_quietScan = false;
        if (scan == m_lastScan)
        {
            return;
        }
        SyncGameSelection(-1);
    }
    if (files.empty())
    {
        m_lastScan.clear();
        SetLibraryStatus("");
        RenderEmptyGames("no-roms");
        return;
    }
    ClearCards();
    m_loaded = 0;
    m_libraryLoading = true;
    RenderGameList();
    SetLibraryStatus("Reading games...");

    // Cards appear as the loader's game list cache reports games; update and DLC files never become cards.
    const int gen = m_gameListGen;
    const auto post = [alive = std::weak_ptr<bool>(m_alive), this, gen](std::function<void()> task) {
        UiThread::Instance().Post([alive, this, gen, task = std::move(task)] {
            if (alive.lock() && gen == m_gameListGen)
            {
                task();
            }
        });
    };
    UiLibrary::Listener listener;
    listener.found = [this, post](UiLibrary::Game game) { post([this, game = std::move(game)] { OnGameFound(game); }); };
    listener.progress = [this, post](uint32_t started, uint32_t total) {
        post([this, started, total] {
            SetLibraryStatus(total != 0 ? "Reading games " + std::to_string(started) + " of " + std::to_string(total) : std::string("Reading games..."));
        });
    };
    listener.done = [this, post, scan = std::move(scan)](bool complete) {
        post([this, complete, scan] {
            m_lastScan = complete ? scan : std::vector<std::string>();
            FinishGameList();
        });
    };
    m_library.Scan(std::move(files), std::move(listener));
}

void UiActivity::OnGameFound(const UiLibrary::Game & game)
{
    const auto existing = std::find_if(m_cards.begin(), m_cards.end(), [&game](const Card & card) { return card.path == game.path && card.programId == game.programId; });
    const bool found = existing != m_cards.end();
    Card & card = found ? *existing : m_cards.emplace_back();
    if (card.icon.starts_with("nxmem:"))
    {
        m_renderer.RemoveMemoryImage(card.icon);
    }
    card.path = game.path;
    card.programId = game.programId;
    if (const auto size = m_fileSizes.find(game.path); size != m_fileSizes.end())
    {
        card.size = size->second;
    }
    if (const auto time = m_fileTimes.find(game.path); time != m_fileTimes.end())
    {
        card.modified = time->second;
    }
    card.publisher = game.publisher;
    card.version = game.version;
    card.title = game.title.empty() ? UiUtil::ShortPath(game.path) : game.title;
    card.icon = game.icon.empty() ? "images/nxemu-logo.png" : m_renderer.AddMemoryImage(game.icon);
    card.loading = false;
    const size_t index = static_cast<size_t>(&card - m_cards.data());
    if (!found)
    {
        m_loaded++;
        AppendCardElement(index);
    }
    else if (Rml::Element * element = m_document->GetElementById("game-" + std::to_string(index)))
    {
        element->SetInnerRML(CardInner(card));
    }
    m_layoutDirty = true;
}

void UiActivity::AppendCardElement(size_t index)
{
    Rml::ElementPtr element = m_document->CreateElement("div");
    element->SetClass("game-card", true);
    element->SetId("game-" + std::to_string(index));
    element->SetAttribute("data-game", static_cast<int>(index));
    element->SetInnerRML(CardInner(m_cards[index]));
    m_cards[index].filtered = !MatchesSearch(m_cards[index]);
    element->SetClass("hidden", m_cards[index].filtered);
    m_gameList->AppendChild(std::move(element));
}

void UiActivity::SortCards()
{
    const int sort = UiUtil::ReadInt(NXUISetting::GameListSort);
    const auto byTitle = [](const Card & a, const Card & b) { return UiUtil::Lowercase(a.title) < UiUtil::Lowercase(b.title); };
    std::stable_sort(m_cards.begin(), m_cards.end(), byTitle);
    if (sort == 1)
    {
        std::stable_sort(m_cards.begin(), m_cards.end(), [](const Card & a, const Card & b) {
            // Games without a publisher go last.
            if (a.publisher.empty() != b.publisher.empty())
            {
                return !a.publisher.empty();
            }
            return UiUtil::Lowercase(a.publisher) < UiUtil::Lowercase(b.publisher);
        });
    }
    else if (sort == 2)
    {
        // As on the desktop: the most recently played first, then the games never played, by name.
        PlayTimeRecords & playTime = EmulationSession::GetInstance().PlayTime();
        std::stable_sort(m_cards.begin(), m_cards.end(), [&playTime](const Card & a, const Card & b) {
            return playTime.Of(a.programId).lastPlayed > playTime.Of(b.programId).lastPlayed;
        });
    }
    else if (sort == 3)
    {
        std::stable_sort(m_cards.begin(), m_cards.end(), [](const Card & a, const Card & b) { return a.size > b.size; });
    }
    else if (sort == 4)
    {
        PlayTimeRecords & playTime = EmulationSession::GetInstance().PlayTime();
        std::stable_sort(m_cards.begin(), m_cards.end(), [&playTime](const Card & a, const Card & b) {
            return playTime.Of(a.programId).seconds > playTime.Of(b.programId).seconds;
        });
    }
}

bool UiActivity::MatchesSearch(const Card & card) const
{
    if (m_search.empty())
    {
        return true;
    }
    const std::string search = UiUtil::Lowercase(m_search);
    return UiUtil::Lowercase(card.title).find(search) != std::string::npos || UiUtil::Lowercase(card.publisher).find(search) != std::string::npos;
}

void UiActivity::ApplySearch()
{
    size_t shown = 0;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        Card & card = m_cards[i];
        card.filtered = !MatchesSearch(card);
        if (Rml::Element * element = m_document->GetElementById("game-" + std::to_string(i)))
        {
            element->SetClass("hidden", card.filtered || card.failed);
        }
        shown += card.filtered || card.failed ? 0 : 1;
    }
    if (Rml::Element * empty = m_document->GetElementById("search-empty"))
    {
        const bool none = !m_search.empty() && shown == 0 && !m_cards.empty();
        empty->SetClass("hidden", !none);
        empty->SetInnerRML(none ? "No game matches \"" + UiUtil::Escape(m_search) + "\"" : std::string());
    }
    m_layoutDirty = true;
    SyncGameSelection(shown != 0 ? 0 : -1);
}

void UiActivity::OpenSearch()
{
    CloseSortMenu();
    m_searching = true;
    m_document->GetElementById("games-topbar")->SetClass("hidden", true);
    m_document->GetElementById("search-bar")->SetClass("hidden", false);
    ShowSearchText();
    m_host.Request("beginTextInput", {m_search});
}

void UiActivity::CloseSearch()
{
    if (!m_searching)
    {
        return;
    }
    m_searching = false;
    m_host.Request("endTextInput");
    m_document->GetElementById("games-topbar")->SetClass("hidden", false);
    m_document->GetElementById("search-bar")->SetClass("hidden", true);
    if (!m_search.empty())
    {
        m_search.clear();
        ApplySearch();
    }
}

void UiActivity::ShowSearchText()
{
    m_document->GetElementById("search-text")->SetInnerRML(UiUtil::Escape(m_search));
    m_document->GetElementById("search-placeholder")->SetClass("hidden", !m_search.empty());
    m_document->GetElementById("search-clear")->SetClass("hidden", m_search.empty());
}

void UiActivity::ToggleSortMenu()
{
    if (m_sortMenuOpen)
    {
        CloseSortMenu();
        return;
    }
    const int sort = UiUtil::ReadInt(NXUISetting::GameListSort);
    std::string rml = "<div class=\"menu-title\">Sort by</div>";
    const std::pair<int, const char *> choices[] = {{0, "Name"}, {1, "Publisher"}, {2, "Recently played"}, {4, "Play time"}, {3, "Size"}};
    for (const auto & [value, label] : choices)
    {
        rml += std::string("<div class=\"menu-item\" data-command=\"sort-") + std::to_string(value) + "\"><div class=\"menu-label\">" + label +
               "</div><img class=\"setting-check" + (value == sort ? " on" : "") + "\" src=\"images/check.png\"/></div>";
    }
    const int view = UiUtil::ReadInt(NXUISetting::GameListView);
    rml += "<div class=\"menu-rule\"/><div class=\"menu-title\">View</div>";
    const char * views[] = {"Grid", "List"};
    for (int i = 0; i < 2; i++)
    {
        rml += std::string("<div class=\"menu-item\" data-command=\"view-") + std::to_string(i) + "\"><div class=\"menu-label\">" + views[i] +
               "</div><img class=\"setting-check" + (i == view ? " on" : "") + "\" src=\"images/check.png\"/></div>";
    }
    m_document->GetElementById("sort-menu")->SetInnerRML(rml);
    m_document->GetElementById("sort-menu")->SetClass("hidden", false);
    m_document->GetElementById("sort-backdrop")->SetClass("hidden", false);
    m_sortMenuOpen = true;
}

void UiActivity::CloseSortMenu()
{
    if (!m_sortMenuOpen)
    {
        return;
    }
    m_sortMenuOpen = false;
    m_document->GetElementById("sort-menu")->SetClass("hidden", true);
    m_document->GetElementById("sort-backdrop")->SetClass("hidden", true);
}

void UiActivity::FinishGameList()
{
    m_libraryLoading = false;
    if (m_loaded == 0)
    {
        SetLibraryStatus("");
        RenderEmptyGames("unreadable");
        return;
    }
    std::erase_if(m_cards, [this](const Card & card) {
        if (card.failed && card.icon.starts_with("nxmem:"))
        {
            m_renderer.RemoveMemoryImage(card.icon);
        }
        return card.failed;
    });
    SortCards();
    RenderGameList();
    ApplySearch();
    SetLibraryStatus("");
}

std::string UiActivity::CardInner(const Card & card) const
{
    const bool list = UiUtil::ReadInt(NXUISetting::GameListView) == 1;
    std::string art = card.loading ? "<div class=\"game-icon-ph\"/>" : "<img class=\"game-icon\" src=\"" + UiUtil::Escape(card.icon) + "\"/>";
    const PlayTimeRecords::Record played = EmulationSession::GetInstance().PlayTime().Of(card.programId);
    std::string playTime;
    if (played.seconds >= 60)
    {
        const uint64_t minutes = played.seconds / 60;
        playTime = minutes < 60 ? std::to_string(minutes) + "m" : std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
    }
    if (!list && !playTime.empty() && UiUtil::ReadBool(NXUISetting::ShowCardPlayTime))
    {
        art += "<div class=\"game-played\"><img src=\"images/clock.png\"/><div>" + playTime + "</div></div>";
    }
    std::string details = card.version.empty() ? std::string() : "v" + card.version;
    if (!card.publisher.empty())
    {
        details += (details.empty() ? "" : " \xC2\xB7 ") + card.publisher;
    }
    std::string info;
    if (list || UiUtil::ReadBool(NXUISetting::ShowCardTitle))
    {
        info += "<div class=\"game-title\">" + UiUtil::Escape(card.title) + "</div>";
    }
    if (list)
    {
        std::string line = details;
        if (!playTime.empty() && UiUtil::ReadBool(NXUISetting::ShowCardPlayTime))
        {
            line += (line.empty() ? "" : " \xC2\xB7 ") + playTime + " played";
        }
        if (!line.empty())
        {
            info += "<div class=\"game-details\">" + UiUtil::Escape(line) + "</div>";
        }
    }
    else if (!details.empty() && UiUtil::ReadBool(NXUISetting::ShowCardDetails))
    {
        info += "<div class=\"game-details\">" + UiUtil::Escape(details) + "</div>";
    }
    return "<div class=\"game-art\">" + art + "</div>" + (info.empty() ? std::string() : "<div class=\"game-info\">" + info + "</div>");
}

void UiActivity::ApplyGameListOptions()
{
    m_gameList->SetClass("list", UiUtil::ReadInt(NXUISetting::GameListView) == 1);
    if (!m_libraryLoading && !m_cards.empty())
    {
        RenderGameList();
        ApplySearch();
    }
}

void UiActivity::RenderGameList()
{
    std::string rml;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        const Card & card = m_cards[i];
        rml += "<div class=\"game-card" + std::string(card.failed || card.filtered ? " hidden" : "") + "\" id=\"game-" + std::to_string(i) + "\"";
        if (!card.loading && !card.failed)
        {
            rml += " data-game=\"" + std::to_string(i) + "\"";
        }
        rml += ">" + CardInner(card) + "</div>";
    }
    m_gameList->SetInnerRML(rml);
    m_layoutDirty = true;
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
    const bool list = UiUtil::ReadInt(NXUISetting::GameListView) == 1;
    const int size = std::clamp(UiUtil::ReadInt(NXUISetting::GameCardSize), 0, 2);
    const float gapX = list ? 0.0f : kCardGapX * dp;
    m_columns = list ? 1 : std::max(1, static_cast<int>((width + gapX) / (kCardMinWidths[size] * dp + gapX)));
    const float cardWidth = std::floor((width - gapX * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns));
    const float artSize = list ? kListIconSize * dp : cardWidth;
    int position = 0;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        if (m_cards[i].failed || m_cards[i].filtered)
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
        card->SetProperty(Rml::PropertyId::MarginBottom, Pixels(list ? 0.0f : kCardGapY * dp));
        if (Rml::Element * art = card->GetFirstChild())
        {
            art->SetProperty(Rml::PropertyId::Width, Pixels(artSize));
            art->SetProperty(Rml::PropertyId::Height, Pixels(artSize));
        }
        position++;
    }
}

std::vector<size_t> UiActivity::SelectableGames() const
{
    std::vector<size_t> cards;
    for (size_t i = 0; i < m_cards.size(); i++)
    {
        if (!m_cards[i].loading && !m_cards[i].failed && !m_cards[i].filtered)
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

void UiActivity::MoveGameSelection(const std::string & direction)
{
    if (!ControllerAttached())
    {
        return;
    }
    if (Top() != "gamesPage")
    {
        MoveSettingsSelection(direction);
        return;
    }
    const std::vector<size_t> cards = SelectableGames();
    if (cards.empty())
    {
        return;
    }
    if (m_selectedGame < 0)
    {
        SyncGameSelection(0);
        return;
    }
    int next = m_selectedGame;
    if (direction == "left") next -= 1;
    else if (direction == "right") next += 1;
    else if (direction == "up") next -= m_columns;
    else if (direction == "down") next += m_columns;
    SyncGameSelection(std::clamp(next, 0, static_cast<int>(cards.size()) - 1));
}

void UiActivity::LaunchSelectedGame()
{
    if (!ControllerAttached())
    {
        return;
    }
    if (Top() != "gamesPage")
    {
        ActivateSettingsSelection();
        return;
    }
    const std::vector<size_t> cards = SelectableGames();
    if (m_selectedGame >= 0 && m_selectedGame < static_cast<int>(cards.size()))
    {
        LaunchGame(m_cards[cards[m_selectedGame]]);
    }
}

void UiActivity::OpenGamePropertiesAt(float x, float y)
{
    if (Top() != "gamesPage")
    {
        return;
    }
    for (Rml::Element * element = m_context.GetElementAtPoint({x, y}); element != nullptr; element = element->GetParentNode())
    {
        if (const int game = element->GetAttribute<int>("data-game", -1); game >= 0)
        {
            OpenGameProperties(static_cast<size_t>(game));
            return;
        }
    }
}

void UiActivity::OpenGameProperties(size_t card)
{
    CloseSortMenu();
    m_host.Request("endTextInput");
    if (card >= m_cards.size() || m_cards[card].loading || m_cards[card].failed)
    {
        return;
    }
    m_propertiesGame = m_cards[card];
    m_properties = {};
    m_propertiesReady = false;
    ReloadGameProperties();
    Navigate("gameProperties");
}

// Reads the game's add-ons, saves and caches again, keeping what is on screen until they arrive.
void UiActivity::ReloadGameProperties()
{
    if (m_propertiesGame.path.empty())
    {
        return;
    }
    const int gen = ++m_propertiesGen;
    UiLibrary::ReadProperties(m_propertiesGame.path, m_propertiesGame.programId, [alive = std::weak_ptr<bool>(m_alive), this, gen](UiLibrary::Properties properties) {
        UiThread::Instance().Post([alive, this, gen, properties = std::move(properties)]() mutable {
            if (!alive.lock() || gen != m_propertiesGen)
            {
                return;
            }
            m_properties = std::move(properties);
            m_propertiesReady = true;
            if (Top().starts_with("game") && Top() != "gameFolders")
            {
                RenderCurrentSettings();
            }
        });
    });
}

void UiActivity::LaunchGame(const Card & card)
{
    // A scan holds the loader until it finishes; the game should not wait for it.
    if (m_libraryLoading)
    {
        m_library.Cancel();
        m_libraryLoading = false;
        m_lastScan.clear();
        SetLibraryStatus("");
    }
    m_host.Request("launchGame", {card.path, std::to_string(card.programId)});
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

void UiActivity::MoveSettingsSelection(const std::string & direction)
{
    const std::vector<Rml::Element *> rows = SettingsRows();
    if (rows.empty())
    {
        return;
    }
    if (direction == "left" || direction == "right")
    {
        NudgeSelectedSlider(direction);
        return;
    }
    if (m_settingsSelection < 0)
    {
        SyncSettingsSelection(0);
        return;
    }
    const int next = m_settingsSelection + (direction == "up" ? -1 : 1);
    SyncSettingsSelection(std::clamp(next, 0, static_cast<int>(rows.size()) - 1));
}

void UiActivity::NudgeSelectedSlider(const std::string & direction)
{
    const std::vector<Rml::Element *> rows = SettingsRows();
    if (m_settingsSelection < 0 || m_settingsSelection >= static_cast<int>(rows.size()))
    {
        return;
    }
    auto * input = dynamic_cast<Rml::ElementFormControlInput *>(rows[m_settingsSelection]->QuerySelector("input"));
    if (input == nullptr)
    {
        return;
    }
    const float min = input->GetAttribute<float>("min", 0.0f);
    const float max = input->GetAttribute<float>("max", 1.0f);
    const float stepAttribute = input->GetAttribute<float>("step", 1.0f);
    const float span = max - min;
    const float step = stepAttribute >= 1 ? stepAttribute : (span > 0 ? span / 20 : 0.05f);
    float value = std::strtof(input->GetValue().c_str(), nullptr) + (direction == "right" ? step : -step);
    if (stepAttribute >= 1)
    {
        value = std::round(value / stepAttribute) * stepAttribute;
    }
    value = std::clamp(value, min, max);
    input->SetValue(std::to_string(value));
}

void UiActivity::ActivateSettingsSelection()
{
    const std::vector<Rml::Element *> rows = SettingsRows();
    if (m_settingsSelection < 0 || m_settingsSelection >= static_cast<int>(rows.size()))
    {
        return;
    }
    Rml::Element * row = rows[m_settingsSelection];
    if (row->QuerySelector("input") != nullptr)
    {
        return;
    }
    if (row->IsClassSet("folder-item"))
    {
        if (Rml::Element * remove = row->QuerySelector(".folder-remove"))
        {
            RunAction(remove);
        }
        return;
    }
    RunAction(row);
}
