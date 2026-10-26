#include "firmware_dialog.h"
#include <RmlUi/Core/ElementDocument.h>
#include <SDL.h>
#include <common/path.h>
#include <common/std_string.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>

namespace
{
constexpr const char * kSteps[] = {"source", "install", "done"};
constexpr uint64_t kStepSettleMs = 400;

std::string Megabytes(uint64_t bytes)
{
    return Tr("%1 MB", {stdstr_f("%.0f", static_cast<double>(bytes) / (1024.0 * 1024.0))});
}

std::string InstalledVersion(ISystemloader & loader)
{
    char version[64]{};
    return std::string(version, loader.GetInstalledFirmwareDisplayVersion(version, sizeof(version)));
}

struct Outcome
{
    std::string title;
    std::string text;
};

Outcome OutcomeOf(FirmwareInstallStatus status)
{
    switch (status)
    {
    case FirmwareInstallStatus::Installed: return {Tr("Firmware installed"), Tr("Games can use it from now on.")};
    case FirmwareInstallStatus::Cancelled: return {Tr("Installation cancelled"), Tr("The installed firmware was left as it was.")};
    case FirmwareInstallStatus::SourceUnreadable:
        return {Tr("The source could not be read"), Tr("Check that the file or the folder is still there and that NXEmu may read it.")};
    case FirmwareInstallStatus::NotFirmware:
        return {Tr("No firmware found there"),
                Tr("Choose a firmware .zip archive, a .dxci game card image that carries an update, or a folder of the firmware's .nca files.")};
    case FirmwareInstallStatus::StorageUnavailable:
        return {Tr("The console's storage cannot be written"), Tr("Check that NXEmu may write to its data folder, then try again.")};
    case FirmwareInstallStatus::ReplaceFailed:
        return {Tr("The firmware could not be replaced"), Tr("The installed firmware was put back. The log tells what went wrong.")};
    default:
        return {Tr("Copying the firmware failed"),
                Tr("The installed firmware was left as it was. Check the free disk space; the log tells what went wrong.")};
    }
}
}

FirmwareDialog::FirmwareDialog(UiWindow & window, ISystemloader & loader, const std::filesystem::path & assetDir, Host host) :
    Dialog(window.Context(), assetDir / "firmware_dialog.rml"),
    m_window(window),
    m_loader(&loader),
    m_host(std::move(host))
{
}

bool FirmwareDialog::OfferSource(const std::string & utf8Path)
{
    if (!IsVisible() || m_installer.Current().phase != FirmwareInstaller::Phase::Idle)
    {
        return false;
    }
    Install(utf8Path);
    return true;
}

void FirmwareDialog::OfferFromGame(const std::string & utf8Path, const std::string & version)
{
    m_nextGame = utf8Path;
    m_nextGameVersion = version;
    Show();
}

void FirmwareDialog::OnOpen()
{
    m_installer.Reset();
    m_shownPhase = FirmwareInstaller::Phase::Idle;
    m_game.clear();
    m_game.swap(m_nextGame);
    m_gameVersion.clear();
    m_gameVersion.swap(m_nextGameVersion);
    SetText("done-button", m_game.empty() ? Tr("Close") : Tr("Start the game"));
    if (m_game.empty())
    {
        ShowStep("source", false);
        ShowInstalledVersion();
        return;
    }
    const std::string installed = InstalledVersion(*m_loader);
    ShowStep("offer", false);
    SetText("offer-title", Tr("This game carries firmware %1", {m_gameVersion}));
    SetText("offer-text", installed.empty() ? Tr("No firmware is installed yet. NXEmu can install the game's before starting it.")
                                            : Tr("Firmware %1 is installed, and the game was made for a more recent one. NXEmu can install the game's before starting it.", {installed}));
}

void FirmwareDialog::OnUpdate()
{
    const FirmwareInstaller::State state = m_installer.Current();
    const bool changed = state.phase != m_shownPhase;
    m_shownPhase = state.phase;
    switch (state.phase)
    {
    case FirmwareInstaller::Phase::Idle:
        if (changed)
        {
            ShowStep("source", false);
            ShowInstalledVersion();
        }
        break;
    case FirmwareInstaller::Phase::Working:
        if (changed)
        {
            ShowStep("install", false);
        }
        ShowProgress(state);
        break;
    case FirmwareInstaller::Phase::Asking:
        if (changed)
        {
            ShowStep("ask", false);
            SetText("ask-text", Tr("The package holds firmware %1, and firmware %2 is installed. Installing the package replaces it.", {state.packageVersion, state.installedVersion}));
        }
        break;
    case FirmwareInstaller::Phase::Done:
        if (changed)
        {
            m_host.installEnded();
            ShowResult(state);
        }
        break;
    }
}

void FirmwareDialog::OnCommand(const std::string & command)
{
    if (SDL_GetTicks64() - m_stepShown < kStepSettleMs)
    {
        return;
    }
    const FirmwareInstaller::Phase phase = m_installer.Current().phase;
    if (command == "pick-file" && phase == FirmwareInstaller::Phase::Idle)
    {
        const std::string filter = FileFilter({{Tr("Firmware"), "*.zip;*.dxci"}, {Tr("All files"), "*.*"}});
        Path selected;
        if (FileSelect(m_window.NativeHandle(), Path(Path::CURRENT_DIRECTORY), filter.c_str(), true, selected))
        {
            Install(static_cast<const char *>(selected));
        }
    }
    else if (command == "pick-folder" && phase == FirmwareInstaller::Phase::Idle)
    {
        const Path selected = BrowseForDirectory(m_window.NativeHandle(), Tr("Choose the folder that holds the firmware").c_str());
        if (!std::string(static_cast<const char *>(selected)).empty())
        {
            Install(static_cast<const char *>(selected));
        }
    }
    else if (command == "install-offer" && phase == FirmwareInstaller::Phase::Idle && !m_game.empty())
    {
        Install(m_game);
    }
    else if (command == "cancel-install")
    {
        m_installer.Cancel();
    }
    else if (command == "replace" || command == "keep")
    {
        m_installer.Answer(command == "replace");
    }
    else if (command == "again" && phase == FirmwareInstaller::Phase::Done)
    {
        m_installer.Reset();
    }
}

void FirmwareDialog::OnCloseRequested()
{
    if (m_installer.Busy())
    {
        m_installer.Cancel();
    }
    else if (SDL_GetTicks64() - m_stepShown >= kStepSettleMs)
    {
        Close();
    }
}

void FirmwareDialog::Install(const std::string & utf8Path)
{
    m_host.installStarting();
    m_installer.Start(*m_loader, utf8Path);
}

void FirmwareDialog::ShowStep(const char * step, bool failed)
{
    Rml::Element * panel = Document()->GetElementById("firmware");
    if (panel == nullptr)
    {
        return;
    }
    for (const char * name : {"source", "offer", "install", "ask", "done"})
    {
        panel->SetClass(std::string("at-") + name, std::string(name) == step);
    }
    panel->SetClass("failed", failed);
    m_stepShown = SDL_GetTicks64();

    const std::string current = std::string(step) == "ask" ? "install" : std::string(step) == "offer" ? "source" : step;
    bool passed = true;
    for (const char * name : kSteps)
    {
        const bool isCurrent = current == name;
        passed = passed && !isCurrent;
        if (Rml::Element * element = Document()->GetElementById(std::string("step-") + name))
        {
            const bool finished = isCurrent && current == "done" && !failed;
            element->SetClass("current", isCurrent && !finished);
            element->SetClass("passed", passed || finished);
        }
    }
}

void FirmwareDialog::ShowInstalledVersion()
{
    const std::string version = InstalledVersion(*m_loader);
    SetText("firmware-installed", version.empty() ? Tr("No firmware is installed yet.") : Tr("Firmware %1 is installed.", {version}));
}

void FirmwareDialog::ShowProgress(const FirmwareInstaller::State & state)
{
    std::string stage, detail;
    switch (state.stage)
    {
    case FirmwareInstallStage::Copying:
        stage = Tr("Copying the firmware...");
        detail = state.total != 0 ? Tr("%1 of %2", {Megabytes(state.done), Megabytes(state.total)}) : std::string();
        break;
    case FirmwareInstallStage::Checking:
        stage = Tr("Checking the firmware...");
        detail = state.total != 0 ? Tr("File %1 of %2", {std::to_string(state.done + 1), std::to_string(state.total)}) : std::string();
        break;
    case FirmwareInstallStage::Replacing:
        stage = Tr("Putting the firmware in place...");
        break;
    }
    SetText("install-stage", stage);
    SetText("install-detail", detail);
    if (Rml::Element * fill = Document()->GetElementById("install-fill"))
    {
        const bool measured = state.total != 0;
        fill->SetClass("indeterminate", !measured);
        if (measured)
        {
            const float percent = 100.0f * static_cast<float>(state.done) / static_cast<float>(state.total);
            fill->SetProperty(Rml::PropertyId::Width, Rml::Property(percent, Rml::Unit::PERCENT));
        }
        else
        {
            fill->RemoveProperty(Rml::PropertyId::Width);
        }
    }
}

void FirmwareDialog::ShowResult(const FirmwareInstaller::State & state)
{
    const bool installed = state.status == FirmwareInstallStatus::Installed;
    const Outcome outcome = OutcomeOf(state.status);
    ShowStep("done", !installed);
    SetText("result-title", installed && !state.packageVersion.empty() ? Tr("Firmware %1 installed", {state.packageVersion}) : outcome.title);
    SetText("result-text", outcome.text);
}
