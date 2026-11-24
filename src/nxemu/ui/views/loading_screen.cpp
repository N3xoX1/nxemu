#include "loading_screen.h"
#include "load_stage.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <optional>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/library/rom_properties.h>

namespace
{
constexpr int kIndeterminate = -1;
constexpr const char * kUnnamed = TrMark("Loading");
constexpr const char * kOpening = TrMark("Opening the game...");
// The game starts with the shaders it uses first, in the order it first used them: the button
// comes once that many are ready (a quarter of a smaller cache), so the start does not stutter.
constexpr int kContinueAfter = 512;
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
    m_canContinue = false;
    m_continueRequested = false;
    m_timeEstimate.Reset();
    SetText("loading-hint", "");
    SetText("loading-eta", "");
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
    const bool compiling = stage == LoadStage::Shaders || stage == LoadStage::BuildProfile;
    SetText("loading-hint", settings.GetBool(NXCoreSetting::ShaderRecordingOff) ? Tr("Shaders are not recorded while mods or cheats are active.") : "");
    std::string remaining;
    std::optional<long long> secondsLeft;
    if (compiling)
    {
        const auto estimate = m_timeEstimate.Update(static_cast<int>(stage), current, total);
        if (estimate)
        {
            secondsLeft = estimate->count();
            const auto seconds = *secondsLeft;
            if (seconds < 60)
                remaining = Tr("About %1 s remaining", {std::to_string(5 * ((seconds + 4) / 5))});
            else
                remaining = Tr("About %1 min remaining", {std::to_string((seconds + 59) / 60)});
        }
        else if (total > 0 && current < total)
            remaining = Tr("Estimating time remaining...");
        else if (total > 0 && current >= total)
            remaining = Tr("Finishing shader preparation...");
    }
    else m_timeEstimate.Reset();
    SetText("loading-eta", remaining.empty() ? remaining : "·  " + remaining);
    const int continueAfter = std::min(kContinueAfter, total / 4);
    m_canContinue = stage == LoadStage::Shaders && total > 0 && current < total && current >= continueAfter;
    if (Rml::Element * button = m_panel->GetElementById("loading-continue"))
    {
        button->SetClass("enabled", m_canContinue && !m_continueRequested);
    }
    std::string action = Tr("Play now, finish in the background");
    if (m_continueRequested)
        action = Tr("Starting the game...");
    else if (stage == LoadStage::Shaders && total > 0 && current < continueAfter)
    {
        // The time to the button, at the pace of the whole estimate.
        if (secondsLeft && total > current)
        {
            const long long seconds = std::max(1LL, *secondsLeft * (continueAfter - current) / (total - current));
            action = seconds < 60 ? Tr("Wait %1 s to play now", {std::to_string(5 * ((seconds + 4) / 5))})
                                  : Tr("Wait %1 min to play now", {std::to_string((seconds + 59) / 60)});
        }
        else
            action = Tr("Preparing the first shaders...");
    }
    SetText("loading-continue", action);
    if (stage == LoadStage::Shaders || stage == LoadStage::ReadProfile ||
        stage == LoadStage::ImportProfile || stage == LoadStage::BuildProfile)
    {
        const char* label = stage == LoadStage::ReadProfile ? TrMark("Reading and verifying shaders") :
                            stage == LoadStage::ImportProfile ? TrMark("Importing shaders") :
                            stage == LoadStage::BuildProfile ? TrMark("Compiling imported shaders") : TrMark("Preparing shaders");
        SetStatus(Tr(label), total > 0 ? std::to_string(std::clamp(current, 0, total)) + " / " + std::to_string(total) : "");
        SetProgress(total > 0 ? static_cast<int>(100.0 * std::clamp(current, 0, total) / total) : kIndeterminate);
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

bool LoadingScreen::RequestContinue()
{
    if (!m_canContinue || m_continueRequested)
    {
        return false;
    }
    m_continueRequested = true;
    Update();
    return true;
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
