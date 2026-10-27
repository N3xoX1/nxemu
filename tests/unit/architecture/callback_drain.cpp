#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
struct Data
{
    SettingsStore *store;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
};
void Blocking(const char *, void *data)
{
    auto &d = *static_cast<Data *>(data);
    d.calls++;
    d.entered = true;
    while (!d.release)
        std::this_thread::yield();
}
void SelfRemoving(const char *setting, void *data)
{
    auto &d = *static_cast<Data *>(data);
    d.calls++;
    d.store->UnregisterCallback(setting, SelfRemoving, &d);
}
void RemoveOther(const char *, void *data)
{
    auto &d = *static_cast<Data *>(data);
    d.entered = true;
    d.store->UnregisterCallback(NXCoreSetting::EmulationState, Blocking, &d);
}
void Throwing(const char *, void *)
{
    throw std::runtime_error("subscriber failure");
}

void CrossCallbackDrain()
{
    SettingsStore store;
    Data d{&store};
    store.RegisterCallback(NXCoreSetting::EmulationState, Blocking, &d);
    store.RegisterCallback(NXCoreSetting::DisplayedFrames, RemoveOther, &d);
    std::thread notifier([&] { store.SetInt(NXCoreSetting::EmulationState, 1); });
    while (!d.entered)
        std::this_thread::yield();
    auto removal =
        std::async(std::launch::async, [&] { store.SetBool(NXCoreSetting::DisplayedFrames, true); });
    assert(removal.wait_for(20ms) == std::future_status::timeout);
    d.release = true;
    notifier.join();
    removal.get();
    store.SetInt(NXCoreSetting::EmulationState, 2);
    assert(d.calls == 1);
}

void ConcurrentUnregister()
{
    SettingsStore store;
    Data d{&store};
    store.RegisterCallback(NXCoreSetting::EmulationState, Blocking, &d);
    std::thread notifier([&] { store.SetInt(NXCoreSetting::EmulationState, 1); });
    while (!d.entered)
        std::this_thread::yield();
    auto remove = [&] { store.UnregisterCallback(NXCoreSetting::EmulationState, Blocking, &d); };
    auto first = std::async(std::launch::async, remove);
    assert(first.wait_for(20ms) == std::future_status::timeout);
    auto second = std::async(std::launch::async, remove);
    assert(second.wait_for(20ms) == std::future_status::timeout);
    d.release = true;
    notifier.join();
    first.get();
    second.get();
}
int main()
{
    CrossCallbackDrain();
    ConcurrentUnregister();
    {
        SettingsStore store;
        store.RegisterCallback(NXCoreSetting::EmulationState, Throwing, nullptr);
        bool threw = false;
        try
        {
            store.SetInt(NXCoreSetting::EmulationState, 1);
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        assert(threw);
        store.UnregisterCallback(NXCoreSetting::EmulationState, Throwing, nullptr);
    }
    for (int j = 0; j < 180; ++j)
    {
        SettingsStore store;
        Data d{&store};
        store.RegisterCallback(NXCoreSetting::EmulationState, Blocking, &d);
        std::thread notifier([&] { store.SetInt(NXCoreSetting::EmulationState, 1); });
        while (!d.entered)
            std::this_thread::yield();
        std::atomic<bool> returned{false};
        std::thread unreg([&] {
            store.UnregisterCallback(NXCoreSetting::EmulationState, Blocking, &d);
            returned = true;
        });
        std::this_thread::sleep_for(50us);
        assert(!returned);
        d.release = true;
        notifier.join();
        unreg.join();
        assert(returned);
        store.SetInt(NXCoreSetting::EmulationState, 2);
        assert(d.calls == 1);
        Data e{&store};
        store.RegisterCallback(NXCoreSetting::EmulationState, SelfRemoving, &e);
        store.SetInt(NXCoreSetting::EmulationState, 3);
        store.SetInt(NXCoreSetting::EmulationState, 4);
        assert(e.calls == 1);
    }
    std::cout << "PASS real SettingsStore: 180 drain/cancel/reentrant callback cycles\n";
}
