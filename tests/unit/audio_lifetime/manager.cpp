// AudioManager callbacks: teardown racing the audio thread, and a replaced manager.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include "core/hle/result.h"
#include "yuzu_audio_core/audio_manager.h"

namespace {

using AudioCore::AudioManager;
using AudioCore::Event;

struct Owner : std::enable_shared_from_this<Owner> {
    explicit Owner(const std::shared_ptr<AudioManager>& manager) : audio{manager} {}
    ~Owner() {
        if (auto manager = audio.lock()) {
            manager->ClearOutManager(this);
        }
        alive = false;
    }
    void Link() {
        const auto result = audio.lock()->SetOutManager(
            [self = weak_from_this()] {
                if (auto owner = self.lock()) {
                    owner->Callback();
                }
            },
            this);
        if (result.IsError()) {
            std::abort();
        }
    }
    void Callback() {
        if (!alive) {
            std::abort();
        }
        std::scoped_lock l{mutex};
        ++calls;
    }

    std::weak_ptr<AudioManager> audio;
    std::recursive_mutex mutex;
    std::atomic<int> calls{};
    std::atomic<bool> alive{true};
};

} // namespace

int main() {
    auto audio = std::make_shared<AudioManager>();
    std::atomic<bool> stop{false};
    std::thread signaller([&] {
        while (!stop) {
            audio->SetEvent(Event::Type::AudioOutManager, true);
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 2000; ++i) {
        auto owner = std::make_shared<Owner>(audio);
        owner->Link();
        if (i % 2) {
            std::thread([owner = std::move(owner)]() mutable { owner.reset(); }).join();
        }
    }

    // Destroying the old owner must leave its replacement registered.
    auto old_owner = std::make_shared<Owner>(audio);
    old_owner->Link();
    auto new_owner = std::make_shared<Owner>(audio);
    new_owner->Link();
    old_owner.reset();
    const int before = new_owner->calls;
    for (int i = 0; i < 400 && new_owner->calls == before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool replacement_fires = new_owner->calls > before;

    stop = true;
    signaller.join();
    audio->Shutdown();
    return replacement_fires ? 0 : 1;
}
