#include "loading_screen.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/rom_properties.h>

namespace
{
enum class LoadStage
{
    Game = 0,
    Shaders = 1,
    Launch = 2,
};

constexpr int kIndeterminate = -1;
constexpr const char * kUnnamed = TrMark("Loading");
constexpr const char * kOpening = TrMark("Opening the game...");
} // namespace

LoadingScreen::LoadingScreen(Rml::Element * panel, UiRenderer & renderer) :
    m_panel(panel),
    m_renderer(renderer)
{
}

LoadingScreen::~LoadingScreen()
{
    Reset();
}

void LoadingScreen::Reset()
{
    for (const char * id : {"loading-logo", "loading-icon", "loading-banner"})
    {
        if (Rml::Element * image = m_panel->GetElementById(id))
        {
            image->RemoveAttribute("src");
            image->SetClass("shown", false);
        }
    }
    for (const std::string & source : m_images)
    {
        m_renderer.RemoveMemoryImage(source);
    }
    m_images.clear();
    m_title.clear();
    m_hasGame = false;
}

void LoadingScreen::Announce(const std::string & title, const std::string & iconSource)
{
    Reset();
    m_title = title;
    m_announcedTick = SettingsStore::GetInstance().GetInt(NXCoreSetting::DiskCacheLoadTick);
    if (Rml::Element * image = m_panel->GetElementById("loading-icon"); image != nullptr && !iconSource.empty())
    {
        image->SetAttribute("src", iconSource);
        image->SetClass("shown", true);
    }
    SetText("loading-title", m_title.empty() ? Tr(kUnnamed) : m_title);
    SetStatus(Tr(kOpening));
    SetProgress(kIndeterminate);
}

void LoadingScreen::SetGame(IRomInfo & info)
{
    if (m_title.empty())
    {
        m_title = RomProperties::Title(info);
        SetText("loading-title", m_title.empty() ? Tr(kUnnamed) : m_title);
    }
    SetImage("loading-logo", RomProperties::Logo(info));
    if (Rml::Element * icon = m_panel->GetElementById("loading-icon"); icon != nullptr && !icon->IsClassSet("shown"))
    {
        SetImage("loading-icon", RomProperties::Icon(info));
    }
    SetImage("loading-banner", RomProperties::Banner(info));
    m_hasGame = true;
}

void LoadingScreen::Update()
{
    SetText("loading-title", m_title.empty() ? Tr(kUnnamed) : m_title);
    SettingsStore & settings = SettingsStore::GetInstance();
    const bool reported = settings.GetInt(NXCoreSetting::DiskCacheLoadTick) != m_announcedTick;
    const LoadStage stage = reported ? static_cast<LoadStage>(settings.GetInt(NXCoreSetting::DiskCacheLoadStage)) : LoadStage::Game;
    const int current = settings.GetInt(NXCoreSetting::DiskCacheLoadCurrent);
    const int total = settings.GetInt(NXCoreSetting::DiskCacheLoadTotal);
    if (stage == LoadStage::Shaders && total > 0)
    {
        SetStatus(Tr("Preparing shaders"), std::to_string(current) + " / " + std::to_string(total));
        SetProgress(static_cast<int>(100.0 * current / total));
    }
    else if (stage == LoadStage::Launch)
    {
        SetStatus(Tr("Starting..."));
        SetProgress(100);
    }
    else
    {
        SetStatus(Tr(kOpening));
        SetProgress(kIndeterminate);
    }
}

void LoadingScreen::SetImage(const char * id, std::vector<uint8_t> encoded)
{
    Rml::Element * image = m_panel->GetElementById(id);
    if (image == nullptr || encoded.empty())
    {
        return;
    }
    m_images.push_back(m_renderer.AddMemoryImage(std::move(encoded)));
    image->SetAttribute("src", m_images.back());
    image->SetClass("shown", true);
}

void LoadingScreen::SetText(const char * id, const std::string & text)
{
    Rml::Element * element = m_panel->GetElementById(id);
    const Rml::String rml = Rml::StringUtilities::EncodeRml(text);
    if (element != nullptr && element->GetInnerRML() != rml)
    {
        element->SetInnerRML(rml);
    }
}

void LoadingScreen::SetStatus(const std::string & text, const std::string & count)
{
    SetText("loading-text", text);
    SetText("loading-count", count);
}

void LoadingScreen::SetProgress(int percent)
{
    Rml::Element * fill = m_panel->GetElementById("loading-fill");
    if (fill == nullptr)
    {
        return;
    }
    const bool indeterminate = percent == kIndeterminate;
    fill->SetClass("indeterminate", indeterminate);
    if (indeterminate)
    {
        fill->RemoveProperty(Rml::PropertyId::Width);
    }
    else
    {
        fill->SetProperty(Rml::PropertyId::Width, Rml::Property(static_cast<float>(std::clamp(percent, 0, 100)), Rml::Unit::PERCENT));
    }
}
