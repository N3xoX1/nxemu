#include "ui_library.h"
#include "emulation_session.h"

#include <thread>

UiLibrary::~UiLibrary()
{
    Cancel();
}

namespace
{
    class ScanCallback final : public IGameListScanCallback
    {
    public:
        ScanCallback(const UiLibrary::Listener & listener, const std::atomic_bool & cancelled) :
            m_listener(listener),
            m_cancelled(cancelled)
        {
        }

        bool ScanStopRequested() override { return m_cancelled; }

        void GameFound(const char * path, IRomInfo * romInfo, bool /*changed*/) override
        {
            if (romInfo == nullptr)
            {
                return;
            }
            UiLibrary::Game game;
            game.path = path;
            if (romInfo->ReadProgramId(game.programId) != LoaderResultStatus::Success)
            {
                game.programId = 0;
            }
            game.title = EmulationSession::RomTitle(romInfo);
            game.icon = EmulationSession::RomIcon(romInfo);
            romInfo->Release();
            if (!m_cancelled)
            {
                m_listener.found(std::move(game));
            }
        }

        void ScanProgress(uint32_t filesStarted, uint32_t filesTotal) override
        {
            if (!m_cancelled)
            {
                m_listener.progress(filesStarted, filesTotal);
            }
        }

    private:
        const UiLibrary::Listener & m_listener;
        const std::atomic_bool & m_cancelled;
    };
}

void UiLibrary::Scan(std::vector<File> files, Listener listener)
{
    Cancel();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancelled = cancelled;
    std::thread([files = std::move(files), listener = std::move(listener), cancelled] {
        std::vector<GameFileEntry> entries;
        entries.reserve(files.size());
        for (const File & file : files)
        {
            entries.push_back({file.path.c_str(), file.size, file.modified});
        }
        ScanCallback callback(listener, *cancelled);
        const bool complete = EmulationSession::GetInstance().ScanGameFiles(entries, callback);
        if (!*cancelled)
        {
            listener.done(complete);
        }
    }).detach();
}

void UiLibrary::ReadProperties(std::string path, uint64_t programId, std::function<void(Properties)> receiver)
{
    std::thread([path = std::move(path), programId, receiver = std::move(receiver)] {
        receiver(EmulationSession::GetInstance().QueryGameProperties(path, programId));
    }).detach();
}

void UiLibrary::Cancel()
{
    if (m_cancelled)
    {
        *m_cancelled = true;
        m_cancelled.reset();
    }
}
