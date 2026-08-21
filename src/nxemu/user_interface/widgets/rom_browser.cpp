#include "rom_browser.h"
#include "user_interface/html_utils.h"
#include "user_interface/app_events.h"
#include "user_interface/sciter_main_window.h"
#include "settings/ui_settings.h"
#include <map>
#include <common/path.h>
#include <common/std_string.h>  
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu/settings/ui_identifiers.h>
#include <sciter_handler.h>
#include <sciter_element.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs.h>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>

namespace
{
enum
{
    TIMER_UPDATE_UI = 1,
};

class WidgetRomBrowser;

struct RomEntry
{
    SciterElement romCard;
    std::string title;
    std::vector<uint8_t> icon;
    bool newItem;
    bool found;
};
using RomEntrys = std::unordered_map<std::string, RomEntry>;

class RomListWorker
{
public:
    RomListWorker(WidgetRomBrowser & romBrowser, ISystemModules & modules);
    ~RomListWorker();

    void Start();
    void Stop();

private:
    void Run();
    void ScanFileSystem(const std::string & dir_path);
    bool HasSupportedFileExtension(const std::string & file_name);

    ISystemModules & m_modules;
    WidgetRomBrowser & m_romBrowser;
    std::mutex & m_romsMutex;
    RomEntrys & m_roms;
    SciterElement & m_rootElement;
    std::thread m_thread;
    bool m_stop;
};

class WidgetRomBrowser :
    public std::enable_shared_from_this<WidgetRomBrowser>,
    public IWidget,
    public IEventSink,
    public ITimerSink,
    public IClickSink,
    public IDoubleClickSink,
    public IRomBrowser
{
    friend class RomListWorker;

    typedef std::map<IWidget *, std::shared_ptr<WidgetRomBrowser>> RomBrowsers;

public:
    static bool Register(ISciterUI & sciterUI);

    // IEventSink
    bool OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t event_code, uint64_t reason) override;

    // ITimerSink
    bool OnTimer(SCITER_ELEMENT Element, uint32_t * TimerId) override;

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // IDoubleClickSink
    bool OnDoubleClick(SCITER_ELEMENT element, SCITER_ELEMENT source) override;

private:
    WidgetRomBrowser() = delete;
    WidgetRomBrowser(const WidgetRomBrowser &) = delete;
    WidgetRomBrowser & operator=(const WidgetRomBrowser &) = delete;

    WidgetRomBrowser(ISciterUI & sciterUI);
    bool RenderUI();
    void SelectRomCard(const SciterElement & card);
    SciterElement FindRomCard(SCITER_ELEMENT start) const;
    std::string GetContextMenuItemId(SCITER_ELEMENT start) const;
    bool HandleContextMenuCommand(const std::string & itemId);
    void AttachContextMenuHandlers();

    // IWidget
    void Attached(SCITER_ELEMENT element, IBaseElement * baseElement) override;
    void Detached(SCITER_ELEMENT element) override;
    std::shared_ptr<void> GetInterface(const char * riid) override;

    // IRomBrowser
    void PopulateAsync() override;
    void SetMainWindow(SciterMainWindow * window, ISystemModules * modules) override;
    void ClearItems() override;

    static void SettingChanged(const char * setting, void * userData);
    static IWidget * __stdcall CreateWidget(ISciterUI & sciterUI);
    static void __stdcall ReleaseWidget(IWidget * widget);

    ISciterUI & m_sciterUI;
    IBaseElement * m_baseElement;
    SciterMainWindow * m_window;
    ISystemModules * m_modules;
    SciterElement m_rootElement;
    SciterElement m_gameTitle;
    SciterElement m_gameGrid;
    SciterElement m_configButton;
    SciterElement m_currentGame;
    std::string m_contextMenuPath;
    std::mutex m_romsMutex;
    RomEntrys m_roms;
    std::unique_ptr<RomListWorker> m_currentWorker;
    bool m_updatingUI;
    static RomBrowsers m_instances;
};

WidgetRomBrowser::RomBrowsers WidgetRomBrowser::m_instances;

WidgetRomBrowser::WidgetRomBrowser(ISciterUI & sciterUI) :
    m_sciterUI(sciterUI),
    m_updatingUI(false),
    m_baseElement(nullptr),
    m_window(nullptr),
    m_modules(nullptr)
{
}

bool WidgetRomBrowser::Register(ISciterUI & sciterUI)
{
    const char * WidgetCss =
        "rombrowser {"
        "    display: block;"
        "    behavior: rombrowser;"
        "}";
    return sciterUI.RegisterWidgetType("rombrowser", WidgetRomBrowser::CreateWidget, WidgetRomBrowser::ReleaseWidget, WidgetCss);
}

bool WidgetRomBrowser::RenderUI()
{
    if (!m_gameTitle.IsValid())
    {
        m_gameTitle.Create("div", "");
        m_gameTitle.SetAttribute("class", "games-title");
        SciterElement title;
        title.Create("h1", "My Games");
        m_gameTitle.Insert(title, m_gameTitle.GetChildCount());

        SciterElement buttonsContainer;
        buttonsContainer.Create("div", "");
        buttonsContainer.SetAttribute("class", "buttons-container");

        m_configButton.Create("div", "");
        m_configButton.SetAttribute("class", "config-button");
        buttonsContainer.Insert(m_configButton, buttonsContainer.GetChildCount());
        m_gameTitle.Insert(buttonsContainer, m_gameTitle.GetChildCount());
        m_rootElement.Insert(m_gameTitle, m_rootElement.GetChildCount());

        std::string ConfigSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 36 36">
  <path d="M18.1 11c-3.9 0-7 3.1-7 7s3.1 7 7 7 7-3.1 7-7-3.1-7-7-7zm0 12c-2.8 0-5-2.2-5-5s2.2-5 5-5 5 2.2 5 5-2.2 5-5 5z" class="outline"/>
  <path d="m32.8 14.7-2.8-.9-.6-1.5 1.4-2.6c.3-.6.2-1.4-.3-1.9l-2.4-2.4c-.5-.5-1.3-.6-1.9-.3l-2.6 1.4-1.5-.6-.9-2.8C21 2.5 20.4 2 19.7 2h-3.4c-.7 0-1.3.5-1.4 1.2L14 6c-.6.1-1.1.3-1.6.6L9.8 5.2c-.6-.3-1.4-.2-1.9.3L5.5 7.9c-.5.5-.6 1.3-.3 1.9l1.3 2.5c-.2.5-.4 1.1-.6 1.6l-2.8.9c-.6.2-1.1.8-1.1 1.5v3.4c0 .7.5 1.3 1.2 1.5l2.8.9.6 1.5-1.4 2.6c-.3.6-.2 1.4.3 1.9l2.4 2.4c.5.5 1.3.6 1.9.3l2.6-1.4 1.5.6.9 2.9c.2.6.8 1.1 1.5 1.1h3.4c.7 0 1.3-.5 1.5-1.1l.9-2.9 1.5-.6 2.6 1.4c.6.3 1.4.2 1.9-.3l2.4-2.4c.5-.5.6-1.3.3-1.9l-1.4-2.6.6-1.5 2.9-.9c.6-.2 1.1-.8 1.1-1.5v-3.4c0-.7-.5-1.4-1.2-1.6zm-.8 4.7-3.6 1.1-.1.5-.9 2.1-.3.5 1.8 3.3-2 2-3.3-1.8-.5.3c-.7.4-1.4.7-2.1.9l-.5.1-1.1 3.6h-2.8l-1.1-3.6-.5-.1-2.1-.9-.5-.3-3.3 1.8-2-2 1.8-3.3-.3-.5c-.4-.7-.7-1.4-.9-2.1l-.1-.5L4 19.4v-2.8l3.4-1 .2-.5c.2-.8.5-1.5.9-2.2l.3-.5-1.7-3.3 2-2 3.2 1.8.5-.3c.7-.4 1.4-.7 2.2-.9l.5-.2L16.6 4h2.8l1.1 3.5.5.2c.7.2 1.4.5 2.1.9l.5.3 3.3-1.8 2 2-1.8 3.3.3.5c.4.7.7 1.4.9 2.1l.1.5 3.6 1.1v2.8z" class="outline"/>
</svg>)";
        m_configButton.SetHTML((uint8_t *)ConfigSvg.c_str(), ConfigSvg.size());
        m_sciterUI.AttachHandler(m_configButton, IID_ICLICKSINK, (IClickSink *)this);
        m_sciterUI.AttachHandler(m_configButton, IID_IDBLCLICKSINK, (IDoubleClickSink *)this);
    }
    if (!m_gameGrid.IsValid())
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        m_gameGrid.Create("div", "");
        m_gameGrid.SetAttribute("class", "games-grid");
        m_gameGrid.SetAttribute("data-size", stdstr_f("%d", settings.GetInt(NXUISetting::MyGameIconSize)).c_str());
        m_rootElement.Insert(m_gameGrid, m_rootElement.GetChildCount());
    }

    if (m_roms.size() > 0)
    {
        std::lock_guard<std::mutex> lock(m_romsMutex);
        for (std::pair<const std::string, RomEntry> & entry : m_roms)
        {
            RomEntry & romEntry = entry.second;
            SciterElement & romCard = romEntry.romCard;
            if (romCard.IsValid())
            {
                continue;
            }
            romCard.Create("div", "");
            romCard.SetAttribute("class", "rom-card");
            romCard.SetAttribute("data-path", entry.first.c_str());
            m_gameGrid.Insert(romCard, m_gameGrid.GetChildCount());
            m_sciterUI.AttachHandler(romCard, IID_ICLICKSINK, (IClickSink *)this);
            m_sciterUI.AttachHandler(romCard, IID_IDBLCLICKSINK, (IDoubleClickSink *)this);
            m_sciterUI.AttachHandler(romCard, IID_EVENTSINK, (IEventSink *)this);
            
            SciterElement romCardContents;
            romCardContents.Create("div", "");
            romCardContents.SetAttribute("class", "rom-card-content");
            romCard.Insert(romCardContents, romCard.GetChildCount());

            if (romEntry.icon.size() > 0)
            {
                SciterElement iconElement;
                iconElement.Create("img", "");
                iconElement.SetAttribute("class", "rom-icon");
                iconElement.SetAttribute("src", ImageDataUri(romEntry.icon.data(), romEntry.icon.size()).c_str());
                romCardContents.Insert(iconElement, romCardContents.GetChildCount());
                romEntry.icon.clear();
            }
        }
    }
    m_updatingUI = false;
    return false;
}

bool WidgetRomBrowser::OnEvent(SCITER_ELEMENT /*element*/, SCITER_ELEMENT source, uint32_t event_code, uint64_t /*reason*/)
{
    if (event_code == EVENT_UPDATE_LIST && !m_updatingUI)
    {
        m_updatingUI = true;
        m_rootElement.SetTimer(250, (uint32_t *)TIMER_UPDATE_UI);
        return false;
    }

    if (event_code == (uint32_t)SciterBehaviorEvent::ContextMenuRequest)
    {
        SciterElement card = FindRomCard(source);
        if (card.IsValid())
        {
            SelectRomCard(card);
            m_contextMenuPath = card.GetAttribute("data-path");
        }
        return false;
    }
    return false;
}

bool WidgetRomBrowser::OnTimer(SCITER_ELEMENT /*element*/, uint32_t * timerId)
{
    if (timerId == (uint32_t *)TIMER_UPDATE_UI)
    {
        return RenderUI();
    };
    return true;
}

bool WidgetRomBrowser::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement el(element);
    std::string className = el.GetAttribute("class");
    if (strcmp(className.c_str(), "config-button") == 0 && m_window != nullptr)
    {
        m_window->ShowConfig("GameBrowser");
        return true;
    }
    else if (strcmp(className.c_str(), "rom-card") == 0)
    {
        SelectRomCard(el);
        return false;
    }
    return HandleContextMenuCommand(GetContextMenuItemId(source));
}

void WidgetRomBrowser::SelectRomCard(const SciterElement & card)
{
    if (!card.IsValid())
    {
        return;
    }
    if (m_currentGame.IsValid())
    {
        m_currentGame.SetState(0, SciterElement::STATE_CURRENT | SciterElement::STATE_VISITED, true);
    }
    m_currentGame = card;
    m_currentGame.SetState(SciterElement::STATE_CURRENT | SciterElement::STATE_VISITED, 0, true);
}

SciterElement WidgetRomBrowser::FindRomCard(SCITER_ELEMENT start) const
{
    SciterElement el(start);
    for (uint32_t i = 0; i < 8 && el.IsValid(); ++i)
    {
        if (strcmp(el.GetAttribute("class").c_str(), "rom-card") == 0)
        {
            return el;
        }
        el = el.GetParent();
    }
    return SciterElement();
}

std::string WidgetRomBrowser::GetContextMenuItemId(SCITER_ELEMENT start) const
{
    SciterElement el(start);
    for (uint32_t i = 0; i < 8 && el.IsValid(); ++i)
    {
        SciterElement parent = el.GetParent();
        if (parent.IsValid() && parent.GetAttribute("id") == "RomCardContextMenu")
        {
            return el.GetAttribute("id");
        }
        el = parent;
    }
    return std::string();
}

bool WidgetRomBrowser::HandleContextMenuCommand(const std::string & itemId)
{
    if (itemId.empty() || m_window == nullptr)
    {
        return false;
    }

    std::string path = m_contextMenuPath;
    if (path.empty() && m_currentGame.IsValid())
    {
        path = m_currentGame.GetAttribute("data-path");
    }
    if (path.empty())
    {
        return true;
    }

    if (itemId == "romStartGame")
    {
        m_window->LoadGame(path.c_str());
        return true;
    }
    if (itemId == "romOpenSaveData")
    {
        m_window->OpenGameSaveDataLocation(path.c_str());
        return true;
    }
    if (itemId == "romOpenModData")
    {
        m_window->OpenGameModDataLocation(path.c_str());
        return true;
    }
    if (itemId == "romProperties")
    {
        m_window->ShowGameConfig(path.c_str());
        return true;
    }

    return false;
}

bool WidgetRomBrowser::OnDoubleClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/)
{
    SciterElement el(element);
    if (strcmp(el.GetAttribute("class").c_str(), "rom-card") == 0 && m_window != nullptr)
    {
        std::string path = el.GetAttribute("data-path");
        m_window->LoadGame(path.c_str());
    }
    return false;
}

void WidgetRomBrowser::SettingChanged(const char * setting, void * userData)
{
    WidgetRomBrowser * impl = (WidgetRomBrowser *)userData;
    SettingsStore & settings = SettingsStore::GetInstance();
    if (strcmp(setting, NXUISetting::MyGameIconSize) == 0 && impl->m_gameGrid)
    {
        impl->m_gameGrid.SetAttribute("data-size", stdstr_f("%d", settings.GetInt(NXUISetting::MyGameIconSize)).c_str());
    }
    else if (strcmp(setting, NXUISetting::GameDirectories) == 0)
    {
        impl->PopulateAsync();
    }
}

IWidget * __stdcall WidgetRomBrowser::CreateWidget(ISciterUI & sciterUI)
{
    std::shared_ptr<WidgetRomBrowser> instance(new WidgetRomBrowser(sciterUI));
    IWidget * widget = (IWidget *)instance.get();
    WidgetRomBrowser * rombrowser = instance.get();
    m_instances.insert(RomBrowsers::value_type(rombrowser, std::move(instance)));
    return widget;
}

void __stdcall WidgetRomBrowser::ReleaseWidget(IWidget * widget)
{
    RomBrowsers::iterator it = m_instances.find(widget);
    if (it != m_instances.end())
    {
        m_instances.erase(it);
    }
}

void WidgetRomBrowser::Attached(SCITER_ELEMENT element, IBaseElement * baseElement)
{
    m_baseElement = baseElement;
    m_rootElement = element;
    m_sciterUI.AttachHandler(m_rootElement, IID_EVENTSINK, (IEventSink *)this);
    m_sciterUI.AttachHandler(m_rootElement, IID_ITIMERSINK, (ITimerSink *)this);
    AttachContextMenuHandlers();

    SettingsStore & settings = SettingsStore::GetInstance();
    settings.RegisterCallback(NXUISetting::MyGameIconSize, WidgetRomBrowser::SettingChanged, this);
    settings.RegisterCallback(NXUISetting::GameDirectories, WidgetRomBrowser::SettingChanged, this);

    RenderUI();
}

void WidgetRomBrowser::Detached(SCITER_ELEMENT /*element*/)
{
    m_baseElement = nullptr;
    m_rootElement = nullptr;
}

std::shared_ptr<void> WidgetRomBrowser::GetInterface(const char * riid)
{
    if (strcmp(riid, IID_ROMBROWSER) == 0)
    {
        return std::static_pointer_cast<IRomBrowser>(shared_from_this());
    }
    return nullptr;
}

void WidgetRomBrowser::PopulateAsync()
{
    if (m_modules != nullptr)
    {    
        m_currentWorker.reset();
        m_currentWorker = std::make_unique<RomListWorker>(*this, *m_modules);
        m_currentWorker->Start();
    }
}

void WidgetRomBrowser::SetMainWindow(SciterMainWindow * window, ISystemModules * modules)
{
    m_modules = modules;
    m_window = window;
    AttachContextMenuHandlers();
}

void WidgetRomBrowser::AttachContextMenuHandlers()
{
    if (!m_rootElement.IsValid())
    {
        return;
    }

    SciterElement documentRoot = m_rootElement.GetRoot();
    if (!documentRoot.IsValid())
    {
        return;
    }

    SciterElement contextMenu = documentRoot.GetElementByID("RomCardContextMenu");
    if (!contextMenu.IsValid())
    {
        contextMenu = documentRoot.FindFirst("menu#RomCardContextMenu");
    }
    if (!contextMenu.IsValid())
    {
        return;
    }

    m_sciterUI.AttachHandler(contextMenu, IID_EVENTSINK, (IEventSink *)this);
    m_sciterUI.AttachHandler(contextMenu, IID_ICLICKSINK, (IClickSink *)this);

    SciterElement propertiesItem = contextMenu.GetElementByID("romProperties");
    if (!propertiesItem.IsValid())
    {
        propertiesItem = contextMenu.FindFirst("li#romProperties");
    }
    if (propertiesItem.IsValid())
    {
        m_sciterUI.AttachHandler(propertiesItem, IID_ICLICKSINK, (IClickSink *)this);
        m_sciterUI.AttachHandler(propertiesItem, IID_EVENTSINK, (IEventSink *)this);
    }
}

void WidgetRomBrowser::ClearItems()
{
    m_currentWorker->Stop();
    std::lock_guard<std::mutex> lock(m_romsMutex);
    m_roms.clear();
}

RomListWorker::RomListWorker(WidgetRomBrowser & romBrowser, ISystemModules & modules) :
    m_modules(modules),
    m_romBrowser(romBrowser),
    m_romsMutex(romBrowser.m_romsMutex),
    m_roms(romBrowser.m_roms),
    m_rootElement(romBrowser.m_rootElement),
    m_stop(false)
{
}

RomListWorker::~RomListWorker()
{
    Stop();
}

void RomListWorker::Start()
{
    m_thread = std::thread(&RomListWorker::Run, this);
}

void RomListWorker::Stop()
{
    m_stop = true;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void RomListWorker::Run()
{
    {
        std::lock_guard<std::mutex> lock(m_romsMutex);
        for (std::pair<const std::string, RomEntry> & entry : m_roms)
        {
            RomEntry & romEntry = entry.second;
            romEntry.found = false;
        }
    }

    for (const std::string & game_dir : uiSettings.gameDirectories)
    {
        if (m_stop)
        {
            return;
        }
        ScanFileSystem(game_dir);
    }

    if (!m_stop)
    {
        std::lock_guard<std::mutex> lock(m_romsMutex);

        for (RomEntrys::iterator itr = m_roms.begin(); itr != m_roms.end();)
        {
            if (!itr->second.found)            
            {
                SciterElement & romCard = itr->second.romCard;
                if (romCard.IsValid())
                {
                    romCard.Destroy();
                }
                itr = m_roms.erase(itr);
            }
            else
            {
                itr++;
            }
        }
    }
}

void RomListWorker::ScanFileSystem(const std::string & dir_path)
{
    const auto callback = [this](const std::filesystem::path & path) -> bool {
        if (m_stop)
        {
            // Breaks the callback loop.
            return false;
        }
        const std::string physical_name = Common::FS::PathToUTF8String(path);
        const bool is_dir = Common::FS::IsDir(path);
        if (!is_dir && (HasSupportedFileExtension(physical_name)))
        {
            {
                std::lock_guard<std::mutex> lock(m_romsMutex);
                RomEntrys::iterator iter = m_roms.find(physical_name);
                if (iter != m_roms.end())
                {
                    iter->second.found = true;
                    return true;
                }
            }

            ISystemloader & systemloader = m_modules.Systemloader();
            RomEntry entry;
            IRomInfoPtr romInfo(systemloader.RomInfo(physical_name.c_str(), 0, 0));
            if (!romInfo)
            {
                return true;
            }
            entry.newItem = true;
            entry.found = true;

            uint32_t size = 0;
            LoaderResultStatus res = romInfo->ReadIcon(nullptr, &size);
            if (res == LoaderResultStatus::Success && size > 0)
            {
                entry.icon.resize(size);
                romInfo->ReadIcon(entry.icon.data(), &size);
            }

            res = romInfo->ReadTitle(nullptr, &size);
            if (res == LoaderResultStatus::Success)
            {
                entry.title.resize(size);
                res = romInfo->ReadTitle(entry.title.data(), &size);
            }

            {
                std::lock_guard<std::mutex> lock(m_romsMutex);
                m_roms.emplace(physical_name, std::move(entry));
            }
            m_rootElement.PostEvent(EVENT_UPDATE_LIST);
        }
        return true;
    };
    Common::FS::IterateDirEntries(dir_path, callback, Common::FS::DirEntryFilter::File);
}

bool RomListWorker::HasSupportedFileExtension(const std::string & file_name)
{
    static const std::unordered_set<std::string> supported_extensions = {
        "nso", "nro", "nca",
        "dxci", "dnsp", "kip"};
    return supported_extensions.contains(stdstr(Path(file_name).GetExtension()).ToLower());
}
} // namespace

bool Register_WidgetRomBrowser(ISciterUI & sciterUI)
{
    return WidgetRomBrowser::Register(sciterUI);
}