#include "firmware_installer.h"

FirmwareInstaller::~FirmwareInstaller()
{
    Cancel();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void FirmwareInstaller::Start(ISystemloader & loader, std::string utf8Path)
{
    if (Busy())
    {
        return;
    }
    Reset();
    {
        std::lock_guard lock(m_mutex);
        m_state.phase = Phase::Working;
    }
    m_thread = std::thread([this, &loader, path = std::move(utf8Path)] {
        char version[64]{};
        const FirmwareInstallStatus status = loader.InstallFirmwarePackage(path.c_str(), this, version, sizeof(version));
        std::lock_guard lock(m_mutex);
        m_state.phase = Phase::Done;
        m_state.status = status;
        m_state.packageVersion = version;
    });
}

FirmwareInstaller::State FirmwareInstaller::Current() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

bool FirmwareInstaller::Busy() const
{
    std::lock_guard lock(m_mutex);
    return m_state.phase == Phase::Working || m_state.phase == Phase::Asking;
}

void FirmwareInstaller::Answer(bool replace)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_state.phase != Phase::Asking)
        {
            return;
        }
        m_hasAnswer = true;
        m_answer = replace;
        m_state.phase = Phase::Working;
    }
    m_answered.notify_all();
}

void FirmwareInstaller::Cancel()
{
    {
        std::lock_guard lock(m_mutex);
        m_cancel = true;
    }
    m_answered.notify_all();
}

void FirmwareInstaller::Reset()
{
    if (Busy())
    {
        return;
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_state = {};
    m_cancel = m_hasAnswer = m_answer = false;
}

bool FirmwareInstaller::FirmwareInstallProgress(FirmwareInstallStage stage, uint64_t done, uint64_t total)
{
    std::lock_guard lock(m_mutex);
    m_state.stage = stage;
    m_state.done = done;
    m_state.total = total;
    return !m_cancel;
}

bool FirmwareInstaller::ConfirmFirmwareReplacement(const char * packageVersion, const char * installedVersion)
{
    std::unique_lock lock(m_mutex);
    m_state.phase = Phase::Asking;
    m_state.packageVersion = packageVersion;
    m_state.installedVersion = installedVersion;
    m_hasAnswer = false;
    m_answered.wait(lock, [this] { return m_hasAnswer || m_cancel; });
    m_state.phase = Phase::Working;
    return m_hasAnswer && m_answer && !m_cancel;
}
