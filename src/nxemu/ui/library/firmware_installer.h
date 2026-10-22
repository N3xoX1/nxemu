#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nxemu-module-spec/system_loader.h>
#include <string>
#include <thread>

// Installs a firmware package in the background and tells how it goes. The
// loader does the work; nothing else may use it until the installer is done.
class FirmwareInstaller final : public IFirmwareInstallObserver
{
public:
    enum class Phase
    {
        Idle,
        Working,
        Asking, // whether to replace a firmware as recent as the package's
        Done,
    };

    struct State
    {
        Phase phase = Phase::Idle;
        FirmwareInstallStage stage = FirmwareInstallStage::Copying;
        uint64_t done = 0;  // of total, in what the stage counts; total is 0 when it counts nothing
        uint64_t total = 0;
        std::string packageVersion;   // known when asking and when done, if the package was read that far
        std::string installedVersion; // known when asking
        FirmwareInstallStatus status = FirmwareInstallStatus::Installed; // when done
    };

    FirmwareInstaller() = default;
    // Stops an installation in progress and waits for it: the installed
    // firmware is left as it was.
    ~FirmwareInstaller();

    // Does nothing while an installation is in progress. The loader must
    // outlive it.
    void Start(ISystemloader & loader, std::string utf8Path);
    State Current() const;
    bool Busy() const;
    // Answers what Phase::Asking asks.
    void Answer(bool replace);
    // Asks a running installation to stop; it ends as Cancelled shortly after.
    void Cancel();
    // Forgets a finished installation.
    void Reset();

private:
    FirmwareInstaller(const FirmwareInstaller &) = delete;
    FirmwareInstaller & operator=(const FirmwareInstaller &) = delete;

    bool FirmwareInstallProgress(FirmwareInstallStage stage, uint64_t done, uint64_t total) override;
    bool ConfirmFirmwareReplacement(const char * packageVersion, const char * installedVersion) override;

    mutable std::mutex m_mutex;
    std::condition_variable m_answered;
    State m_state;
    bool m_cancel = false;
    bool m_hasAnswer = false;
    bool m_answer = false;
    std::thread m_thread;
};
