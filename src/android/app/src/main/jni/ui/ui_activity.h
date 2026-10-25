#pragma once

#include "ui_library.h"

#include <common/json.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class UiHost;
class UiRenderer;
namespace Rml
{
class Context;
class Element;
class ElementDocument;
class Event;
}

class UiActivity
{
public:
    enum class Mode
    {
        Library,
        Settings,
    };

    UiActivity(Rml::Context & context, const UiHost & host, UiRenderer & renderer, Mode mode);
    ~UiActivity();

    bool Load();
    void Update();
    bool Animating() const;
    void Resized();
    void SetInsets(int left, int top, int right, int bottom);

    void Event(const std::string & name, const std::string & payload);

private:
    UiActivity(const UiActivity &) = delete;
    UiActivity & operator=(const UiActivity &) = delete;

    struct Option
    {
        const char * label;
        int value;
        const char * detail;
    };
    struct Select
    {
        const char * label;
        const char * key;
        std::vector<Option> options;
        std::function<std::string()> format;
        std::function<std::string(const Option &)> optionDetail;
    };
    struct Card
    {
        std::string path;
        std::string title;
        std::string icon;
        bool loading = true;
        bool failed = false;
    };
    struct Choices
    {
        std::string title;
        std::vector<std::string> items;
        std::function<void(size_t)> pick;
        std::string emptyText;
    };
    struct Download
    {
        std::string repo;
        std::string tag;
        std::string asset;
        int percent = 0;
    };

    const std::string & Top() const { return m_navStack.back(); }
    bool IsSettingsPage(const std::string & id) const;
    void Navigate(const std::string & page);
    void GoBack();
    bool HandleBack();
    void ShowPage(const std::string & page);
    std::string SettingsTitle(const std::string & id);
    void RenderCurrentSettings();
    void ApplyTheme();

    void OnClick(Rml::Event & event);
    void OnChange(Rml::Event & event);
    bool RunAction(Rml::Element * element);
    void RunCommand(const std::string & command);
    void OnPad(const std::string & button);
    void OnSettingChanged(const std::string & setting);
    void Confirm(const std::string & message, std::function<void()> accepted);

    void StartApp();
    void RefreshGameList();
    void RescanGameList();
    void OnGameLibraryPaths(const JsonValue & paths);
    void OnGameRead(const UiLibrary::Game & game);
    void FinishGameList();
    void RenderGameList();
    void RenderEmptyGames(const char * kind);
    std::string CardInner(const Card & card) const;
    void ClearCards();
    void SetLibraryStatus(const std::string & text);
    void LayoutGames();
    std::vector<size_t> SelectableGames() const;
    void SyncGameSelection(int index);
    void MoveGameSelection(const std::string & direction);
    void LaunchSelectedGame();
    void LaunchGame(const std::string & path);

    bool ControllerAttached() const;
    void ApplyControllerBrowser();
    void UpdatePlayHint();
    std::vector<Rml::Element *> SettingsRows() const;
    void SyncSettingsSelection(int index);
    void RefreshSettingsSelection();
    void MoveSettingsSelection(const std::string & direction);
    void NudgeSelectedSlider(const std::string & direction);
    void ActivateSettingsSelection();

    std::string Action(std::function<void()> action);
    void BeginBody();
    void CommitBody();
    void ShowFab(std::function<void()> action);
    void AppendHeader(const std::string & text);
    void AppendSub(const std::string & text);
    void AppendLink(const std::string & label, const std::string & detail, std::function<void()> action);
    void AppendBool(const std::string & label, const std::string & detail, bool on, std::function<void()> action);
    void AppendSlider(const std::string & label, const std::string & valueText, float min, float max, float step, float value, std::function<std::string(float)> changed);
    void AppendCheck(const std::string & label, const std::string & detail, bool on, std::function<void()> action);
    void AppendEmptyState(const std::string & title, const std::string & copy, const std::string & button, std::function<void()> action);
    void AppendEmptyHint(const std::string & text);
    void AppendBoolSetting(const char * label, const char * key, const char * detail, bool intBool = false, bool save = true);
    void AppendIntSetting(const char * label, const char * key, int min, int max, const char * suffix);
    void AppendSelectSetting(const Select & select);
    void BuildSelects();
    const Select & SelectFor(const char * key) const;

    void RenderSettingsHome();
    void RenderOverlaySettings();
    void RenderSystemSettings();
    void RenderGraphicsSettings();
    void RenderThemeSettings();
    void RenderSettingChoices();
    void RenderGameFolders();
    void RemoveGameFolder(const std::string & uri);
    void RenderSetup();
    void SetupNext();
    void FinishSetup();
    void AppendSetupIntro(const std::string & copy);
    void AppendSetupNext(const std::string & label, bool done);
    std::string SetupTitle() const;

    void RenderGpuDrivers();
    void RenderGpuDriverAdd();
    void RenderGpuDriverDownload();
    void RenderGpuDriverGroup();
    void RenderGpuDriverRelease();
    void RenderGpuDriversPage();
    bool RenderGpuCatalogState();
    void LoadGpuCatalog(bool force);
    void OnGpuDriverCatalog(const JsonValue & catalog);
    void OnGpuDriverDownload(const JsonValue & event);
    void AppendGpuRecommendation();
    void AppendGpuAssetRow(const std::string & title, const std::string & repo, const std::string & tag, const std::string & asset,
                           int64_t size, const std::string & file, const JsonValue & stored, const std::string & published);
    void AppendGpuDriverRow(const std::string & title, const std::string & subtitle, bool selected, std::function<void()> select,
                            std::function<void()> remove);
    JsonValue GpuDriverList() const;
    JsonValue StoredGpuDriverFiles() const;
    bool GpuDriverSupported() const;
    std::string GpuDriverLabel() const;

    void RenderControllerSettings();
    void RenderControllerPlayer();
    void RenderControllerChoices();
    void RenderControllerPages();
    void RenderControllersSummary();
    void OpenControllerChoices(Choices choices);
    void AppendControllerMap(const std::string & title, const std::string & mapId);
    void AppendStickDirections(int stick);
    void AppendStickExtras(int stick);
    void AppendButtons(std::initializer_list<std::pair<const char *, int>> buttons);
    void AppendStickSlider(const std::string & title, int stick, const char * key, float min, float max);

    Rml::Context & m_context;
    const UiHost & m_host;
    UiRenderer & m_renderer;
    const Mode m_mode;
    Rml::ElementDocument * m_document = nullptr;
    Rml::Element * m_gamesPage = nullptr;
    Rml::Element * m_settingsShell = nullptr;
    Rml::Element * m_gameList = nullptr;
    Rml::Element * m_settingsBody = nullptr;
    Rml::Element * m_settingsScroll = nullptr;
    Rml::Element * m_fab = nullptr;
    int m_themeGeneration = -1;
    int m_insets[4] = {};
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    std::vector<std::string> m_navStack;
    std::vector<std::function<void()>> m_actions;
    std::vector<std::function<std::string(float)>> m_sliders;
    std::string m_body;
    bool m_building = false;
    std::function<void()> m_fabAction;
    std::map<int, std::function<void()>> m_confirms;
    int m_nextConfirm = 0;
    UiLibrary m_library;
    std::vector<Card> m_cards;
    int m_gameListGen = 0;
    bool m_libraryDirty = true;
    bool m_libraryLoading = false;
    bool m_scanning = false;
    bool m_quietScan = false;
    bool m_layoutDirty = false;
    size_t m_loaded = 0;
    int m_columns = 1;
    int m_selectedGame = -1;

    int m_settingsSelection = -1;
    std::map<std::string, int> m_selectionByPage;
    size_t m_setupStep = 0;
    std::vector<std::string> m_setupSteps;

    const Select * m_settingChoice = nullptr;
    std::vector<Select> m_selects;
    JsonValue m_gpuCatalog;
    bool m_gpuCatalogReady = false;
    bool m_gpuLoading = false;
    uint32_t m_gpuGroup = 0;
    uint32_t m_gpuRelease = 0;
    std::unique_ptr<Download> m_gpuDownload;

    int m_controllerPlayer = 0;
    int m_controllerFilter = 0;
    Choices m_choices;
};
