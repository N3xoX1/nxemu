#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace
{
constexpr const char * kSetting = "module_callback_lifetime_test";

void Require(bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

struct BlockingCallbackState
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> invocations{0};
};

void BlockingCallback(const char *, void * data)
{
    auto & state = *static_cast<BlockingCallbackState *>(data);
    ++state.invocations;
    state.entered = true;
    while (!state.release.load())
    {
        std::this_thread::yield();
    }
}

struct SelfRemovingState
{
    SettingsStore & settings;
    int invocations = 0;
};

void SelfRemovingCallback(const char * setting, void * data)
{
    auto & state = *static_cast<SelfRemovingState *>(data);
    ++state.invocations;
    state.settings.UnregisterCallback(setting, SelfRemovingCallback, data);
}

void ThrowingCallback(const char *, void *)
{
    throw std::runtime_error("expected subscriber failure");
}

struct ConcurrentSelfRemovingState
{
    SettingsStore & settings;
    std::latch bothEntered{2};
    std::atomic<int> calls{0};
};

void ConcurrentSelfRemovingCallback(const char * setting, void * data)
{
    auto & state = *static_cast<ConcurrentSelfRemovingState *>(data);
    ++state.calls;
    state.bothEntered.count_down();
    state.bothEntered.wait();
    state.settings.UnregisterCallback(setting, ConcurrentSelfRemovingCallback, data);
}
} // namespace

int main()
{
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        SettingsStore settings;
        BlockingCallbackState state;
        settings.RegisterCallback(kSetting, BlockingCallback, &state);

        std::thread notifier([&] { settings.SetInt(kSetting, 1); });
        while (!state.entered.load())
        {
            std::this_thread::yield();
        }
        auto unregister = std::async(std::launch::async, [&] {
            settings.UnregisterCallback(kSetting, BlockingCallback, &state);
        });
        Require(unregister.wait_for(1ms) == std::future_status::timeout);
        state.release = true;
        notifier.join();
        unregister.get();

        settings.SetInt(kSetting, 2);
        Require(state.invocations.load() == 1);

        SelfRemovingState self{settings};
        settings.RegisterCallback(kSetting, SelfRemovingCallback, &self);
        settings.SetInt(kSetting, 3);
        settings.SetInt(kSetting, 4);
        Require(self.invocations == 1);
    }

    SettingsStore settings;
    settings.RegisterCallback(kSetting, ThrowingCallback, nullptr);
    bool threw = false;
    try
    {
        settings.SetInt(kSetting, 1);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    Require(threw);
    settings.UnregisterCallback(kSetting, ThrowingCallback, nullptr);

    // Two threads can be inside the same self-removing callback. Waiting for
    // the other invocation here would deadlock both threads.
    ConcurrentSelfRemovingState concurrent{settings};
    settings.RegisterCallback(NXCoreSetting::EmulationState, ConcurrentSelfRemovingCallback, &concurrent);
    std::thread first([&] { settings.SetInt(NXCoreSetting::EmulationState, 1); });
    std::thread second([&] { settings.SetInt(NXCoreSetting::EmulationState, 2); });
    first.join();
    second.join();
    Require(concurrent.calls.load() == 2);
    settings.SetInt(NXCoreSetting::EmulationState, 3);
    Require(concurrent.calls.load() == 2);
}
