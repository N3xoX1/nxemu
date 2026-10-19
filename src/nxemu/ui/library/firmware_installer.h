#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nxemu-module-spec/system_loader.h>
#include <string>
#include <thread>

class FirmwareInstaller final : public IFirmwareInstallObserver
{
public:
    enum class Phase
    {
        Idle,
        Working,
        Asking,
        Done,
    };

    struct State
    {
        Phase phase = Phase::Idle;
        FirmwareInstallStage stage = FirmwareInstallStage::Copying;
        uint64_t done = 0;
        uint64_t total = 0;
        std::string packageVersion;
        std::string installedVersion;
        FirmwareInstallStatus status = FirmwareInstallStatus::Installed;
    };

    FirmwareInstaller() = default;
    ~FirmwareInstaller();

    void Start(ISystemloader & loader, std::string utf8Path);
    State Current() const;
    bool Busy() const;
    void Answer(bool replace);
    void Cancel();
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
