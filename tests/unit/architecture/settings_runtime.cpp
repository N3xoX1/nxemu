#include "nxemu-core/settings/identifiers.h"
#include "nxemu-core/settings/settings.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

static std::atomic<int> callback_count{0};
static std::atomic<int> progress_notifications{0};
static void ProgressCallback(const char *, void *user)
{
    auto &store = *static_cast<SettingsStore *>(user);
    const auto tick = store.GetInt(NXCoreSetting::DiskCacheLoadTick);
    assert((static_cast<uint32_t>(tick) & 1u) == 0);
    ++progress_notifications;
}
static void ConcurrentCallback(const char *, void *user)
{
    ++(*static_cast<std::atomic<int> *>(user));
}

static void Callback(const char *, void *user)
{
    auto &store = *static_cast<SettingsStore *>(user);
    assert(store.GetBool(NXCoreSetting::EmulationRunning));
    store.SetInt(NXCoreSetting::EmulationState, 5); // re-entrant notification
    ++callback_count;
}

int main()
{
    SettingsStore store;
    store.RegisterCallback(NXCoreSetting::EmulationRunning, Callback, &store);
    store.SetBool(NXCoreSetting::EmulationRunning, true);
    assert(callback_count == 1 && store.GetInt(NXCoreSetting::EmulationState) == 5);
    store.UnregisterCallback(NXCoreSetting::EmulationRunning, Callback, &store);
    store.SetBool(NXCoreSetting::EmulationRunning, false);
    assert(callback_count == 1);

    store.RegisterCallback(NXCoreSetting::DiskCacheLoadTick, ProgressCallback, &store);
    store.SetInt(NXCoreSetting::DiskCacheLoadTick, 1); // in-progress, no notification
    assert(progress_notifications == 0);
    store.SetInt(NXCoreSetting::DiskCacheLoadCurrent, 11);
    store.SetInt(NXCoreSetting::DiskCacheLoadTotal, 22);
    store.SetInt(NXCoreSetting::DiskCacheLoadTick, 2);
    assert(progress_notifications == 1);
    store.UnregisterCallback(NXCoreSetting::DiskCacheLoadTick, ProgressCallback, &store);
    store.SetInt(NXCoreSetting::DiskCacheLoadTick, 3);
    store.SetInt(NXCoreSetting::DiskCacheLoadStage, 1);
    store.SetInt(NXCoreSetting::DiskCacheLoadCurrent, 0);
    store.SetInt(NXCoreSetting::DiskCacheLoadTotal, 0);
    store.SetInt(NXCoreSetting::DiskCacheLoadTick, 0);

    // Notification registry modification is safe during event publication,
    // and callbacks are allowed to reenter SettingsStore without deadlocking.
    std::atomic<int> notifications{0};
    std::thread subscribe([&] {
        for (int i = 0; i < 4000; ++i)
        {
            store.RegisterCallback(NXCoreSetting::EmulationState, ConcurrentCallback, &notifications);
            store.UnregisterCallback(NXCoreSetting::EmulationState, ConcurrentCallback, &notifications);
        }
    });
    for (int i = 0; i < 4000; ++i)
        store.SetInt(NXCoreSetting::EmulationState, i % 7);
    subscribe.join();

    std::atomic<bool> ready{false};
    constexpr uint32_t N = 180000;
    std::thread writer([&] {
        for (uint32_t i = 1; i <= N; ++i)
        {
            uint32_t prior = static_cast<uint32_t>(store.GetInt(NXCoreSetting::DiskCacheLoadTick));
            uint32_t writing = (prior & ~uint32_t{1}) + 1u;
            store.SetInt(NXCoreSetting::DiskCacheLoadTick, static_cast<int32_t>(writing));
            store.SetInt(NXCoreSetting::DiskCacheLoadStage, 1);
            store.SetInt(NXCoreSetting::DiskCacheLoadCurrent, static_cast<int32_t>(i));
            std::this_thread::yield();
            store.SetInt(NXCoreSetting::DiskCacheLoadTotal, static_cast<int32_t>(2 * i));
            store.SetInt(NXCoreSetting::DiskCacheLoadTick, static_cast<int32_t>(writing + 1u));
            store.SetInt(NXCoreSetting::EmulationState, i % 7);
            store.SetBool(NXCoreSetting::DisplayedFrames, (i % 2) == 1);
        }
        ready = true;
    });
    uint64_t sampled = 0;
    while (!ready || sampled < 1000)
    {
        SettingsStore::DiskCacheProgress progress;
        if (store.TryGetDiskCacheProgress(progress))
        {
            if (progress.tick != 0)
            {
                assert(progress.stage == 1);
                assert(progress.current >= 1);
                assert(progress.total == progress.current * 2);
                assert((static_cast<uint32_t>(progress.tick) & 1u) == 0);
                sampled++;
            }
        }
        (void)store.GetInt(NXCoreSetting::EmulationState);
        (void)store.GetBool(NXCoreSetting::DisplayedFrames);
        std::this_thread::yield();
    }
    writer.join();
    SettingsStore::DiskCacheProgress final;
    assert(store.TryGetDiskCacheProgress(final) && final.current == (int)N && final.total == (int)(N * 2));
    std::cout << "PASS real SettingsStore: coherent progress " << sampled << " snapshots, " << N
              << " writes, callbacks reentrant\n";
}
