#include "ui_library.h"
#include "emulation_session.h"

#include <thread>

UiLibrary::~UiLibrary()
{
    Cancel();
}

void UiLibrary::Read(std::vector<std::string> paths, Receiver receiver)
{
    Cancel();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancelled = cancelled;
    std::thread([paths = std::move(paths), receiver = std::move(receiver), cancelled] {
        for (size_t i = 0; i < paths.size() && !*cancelled; i++)
        {
            Game game;
            game.index = i;
            std::vector<EmulationSession::RomListing> listings;
            game.readable = EmulationSession::GetInstance().QueryRomListing(paths[i], listings) && !listings.empty();
            for (EmulationSession::RomListing & listing : listings)
            {
                game.programs.push_back({listing.programId, std::move(listing.title), std::move(listing.icon)});
            }
            if (!*cancelled)
            {
                receiver(&game);
            }
        }
        if (!*cancelled)
        {
            receiver(nullptr);
        }
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
