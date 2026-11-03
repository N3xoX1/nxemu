#include "about_dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/shell.h>
#include <nxemu/ui/i18n/translation.h>

namespace
{
constexpr int kWidth = 600;
constexpr int kClicksToEnableDebugSettings = 7;
constexpr int kClicksBeforeCountdown = 3;
} // namespace

AboutDialog::AboutDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir) :
    Dialog(window, assetDir / "about_dialog.rml", 620.0f, kWidth, 0),
    m_session(session)
{
}

void AboutDialog::OnOpen()
{
    m_versionClicks = 0;
    SetText("about-version", VER_FILE_VERSION_STR);
    SetText("about-copyright", VER_COPYRIGHT_STR);
    SetText("about-debug", "");
    UiEvents::Listen(Document(), "click", [](Rml::Event & event) {
        Rml::Element * target = event.GetTargetElement();
        if (target->HasAttribute("data-url"))
        {
            OpenUrl(target->GetAttribute<Rml::String>("data-url", ""));
        }
    });
}

void AboutDialog::OnCommand(const std::string & command)
{
    if (command != "version")
    {
        return;
    }
    std::string note;
    if (coreSettings.debugSettingsEnabled)
    {
        note = Tr("Debug settings are already enabled.");
    }
    else if (m_session.IsRunning())
    {
        note = Tr("Stop the game to enable debug settings.");
    }
    else if (++m_versionClicks < kClicksToEnableDebugSettings)
    {
        if (m_versionClicks >= kClicksBeforeCountdown)
        {
            note = TrCount("%n more click to enable debug settings.", "%n more clicks to enable debug settings.", kClicksToEnableDebugSettings - m_versionClicks);
        }
    }
    else
    {
        SettingsStore::GetInstance().SetBool(NXCoreSetting::DebugSettingsEnabled, true);
        SaveCoreSetting();
        note = Tr("Debug settings are enabled, under Debug in the settings.");
    }
    SetText("about-debug", Rml::StringUtilities::EncodeRml(note));
}
