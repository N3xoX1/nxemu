#include "game_config.h"
#include "game_config_addons.h"
#include "user_interface/html_utils.h"
#include "user_interface/notification.h"
#include <common/path.h>
#include <common/std_string.h>
#include <filesystem>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/system_loader.h>
#include <sciter_element.h>
#include <sciter_ui.h>
#include <yuzu_common/fs/filesystem_interfaces.h>

namespace
{
static const char * FileTypeLabel(LoaderFileType type)
{
    switch (type)
    {
    case LoaderFileType::NSO: return "NSO";
    case LoaderFileType::NRO: return "NRO";
    case LoaderFileType::NCA: return "NCA";
    case LoaderFileType::NSP: return "DNSP";
    case LoaderFileType::XCI: return "DXCI";
    case LoaderFileType::NAX: return "NAX";
    case LoaderFileType::KIP: return "KIP";
    case LoaderFileType::DeconstructedRomDirectory: return "Directory";
    default: return "Unknown";
    }
}

static std::string ReadableByteSize(uint64_t size)
{
    static const char * units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    constexpr uint64_t base = 1000;
    if (size == 0)
    {
        return "0 B";
    }
    const size_t maxIndex = (sizeof(units) / sizeof(units[0])) - 1;
    size_t digitGroups = 0;
    double value = (double)size;
    while (value >= (double)base && digitGroups < maxIndex)
    {
        value /= (double)base;
        ++digitGroups;
    }
    return stdstr_f("%.1f %s", value, units[digitGroups]);
}
}

GameConfig::GameConfig(ISciterUI & sciterUI, SystemModules & modules) :
    m_sciterUI(sciterUI),
    m_modules(modules),
    m_window(nullptr),
    m_programId(0)
{
}

GameConfig::~GameConfig() = default;

void GameConfig::Display(void * parentWindow, const char * gamePath)
{
    enum
    {
        WINDOW_WIDTH = 900,
        WINDOW_HEIGHT = 620,
    };

    if (gamePath == nullptr || gamePath[0] == '\0' || !m_modules.IsValid())
    {
        Notification::GetInstance().DisplayError("Unable to open game configuration.", "Configure Game");
        return;
    }

    m_gamePath = gamePath;
    m_programId = 0;
    m_title.clear();
    m_developer.clear();
    m_version = "1.0.0";
    m_format.clear();
    m_size.clear();
    m_filename.clear();
    m_icon.clear();
    m_window = nullptr;
    m_pageNav.reset();
    m_gameConfigAddons.reset();

    Path pathObj(gamePath);
    m_filename = pathObj.GetNameExtension();

    ISystemloader & loader = m_modules.Modules().Systemloader();
    IRomInfoPtr romInfo(loader.RomInfo(gamePath, 0, 0));
    if (romInfo)
    {
        romInfo->ReadProgramId(m_programId);
        m_format = FileTypeLabel(romInfo->GetFileType());
        romInfo->AddToManualContentProvider(loader.ManualContentProvider());

        uint32_t size = 0;
        if (romInfo->ReadTitle(nullptr, &size) == LoaderResultStatus::Success && size > 0)
        {
            m_title.resize(size);
            romInfo->ReadTitle(m_title.data(), &size);
            while (!m_title.empty() && m_title.back() == '\0')
            {
                m_title.pop_back();
            }
        }

        size = 0;
        if (romInfo->ReadIcon(nullptr, &size) == LoaderResultStatus::Success && size > 0)
        {
            m_icon.resize(size);
            romInfo->ReadIcon(m_icon.data(), &size);
        }
    }

    if (m_title.empty())
    {
        m_title = m_filename;
    }
    if (m_format.empty())
    {
        m_format = pathObj.GetExtension();
        if (!m_format.empty() && m_format[0] == '.')
        {
            m_format.erase(m_format.begin());
        }
        for (char & c : m_format)
        {
            c = (char)toupper((unsigned char)c);
        }
        if (m_format.empty())
        {
            m_format = "Unknown";
        }
    }

    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(
        std::filesystem::path(std::u8string(m_gamePath.begin(), m_gamePath.end())), ec);
    if (!ec)
    {
        m_size = ReadableByteSize((uint64_t)fileSize);
    }

    if (m_programId != 0)
    {
        IFileSysNACP * nacp = loader.GetPMControlMetadata(m_programId);
        if (nacp != nullptr)
        {
            const char * version = nacp->GetVersionString();
            if (version != nullptr && version[0] != '\0')
            {
                m_version = version;
            }
            const char * developer = nacp->GetDeveloperName();
            if (developer != nullptr && developer[0] != '\0')
            {
                m_developer = developer;
            }
            nacp->Release();
        }
    }

    if (!m_sciterUI.WindowCreate(parentWindow, "game_config.html", 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SUIW_CHILD, m_window))
    {
        Notification::GetInstance().DisplayError("Unable to create the game configuration dialog.", "Configure Game");
        return;
    }

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        PopulateInfo();

        SciterElement pageNav = root.GetElementByID("GameConfigTabNav");
        std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
        if (interfacePtr)
        {
            m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
            m_pageNav->AddSink(this);
        }
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
}

void GameConfig::PopulateInfo()
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    SetInputValue(root, "display_name", m_title);
    SetInputValue(root, "display_developer", m_developer);
    SetInputValue(root, "display_version", m_version);
    SetInputValue(root, "display_title_id", stdstr_f("%016llX", (unsigned long long)m_programId));
    SetInputValue(root, "display_format", m_format);
    SetInputValue(root, "display_size", m_size);
    SetInputValue(root, "display_filename", m_filename);

    SciterElement iconEl(root.GetElementByID("GameIcon"));
    if (iconEl && !m_icon.empty())
    {
        iconEl.SetAttribute("src", ImageDataUri(m_icon.data(), m_icon.size()).c_str());
    }
}

bool GameConfig::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool GameConfig::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void GameConfig::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "AddOns")
    {
        m_gameConfigAddons.reset(new GameConfigAddons(m_sciterUI, *this, m_modules, page));
    }
}

void GameConfig::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}
