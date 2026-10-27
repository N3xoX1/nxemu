#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "emu_thread.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <nxemu-module-spec/video.h>
#include <thread>
using namespace std::chrono_literals;
struct Settings : IModuleSettings
{
    std::atomic<bool> running{true};
    std::atomic<int> state{-1};
    void SetBool(const char *, bool b) override
    {
        running = b;
    }
    void SetInt(const char *, int32_t v) override
    {
        state = v;
    }
};
Settings settings;
IModuleSettings *g_settings = &settings;
struct Video : IVideo
{
    std::promise<bool> p;
    std::shared_future<bool> fut = p.get_future().share();
    std::atomic<int> waits{0};
    bool WaitForGuestGpu() override
    {
        waits++;
        return fut.get();
    }
};
template <class F> void wait_until(F pred)
{
    for (int i = 0; i < 4000 && !pred(); ++i)
        std::this_thread::sleep_for(50us);
    if (!pred())
    {
        std::cerr << "timed out\n";
        std::abort();
    }
}
void stop_during_active_process(int count)
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess proc;
    Kernel::KProcess *pointer = &proc;
    settings.running = true;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    assert(th->BeginProcessLaunch());
    proc.launch_active = true;
    assert(!th->BeginProcessLaunch()); // A second loader cannot own the same process.
    th->Stop();
    std::this_thread::sleep_for(150us);
    assert(!proc.closed_while_launching && proc.closes == 0 && sys.shutdowns == 0);
    proc.launch_active = false;
    th->ProcessLaunchCompleted(true);
    wait_until([&] { return sys.shutdowns.load() == 1; });
    assert(proc.closes == 1 && !proc.closed_while_launching && pointer == nullptr && sys.runs == 0);
    assert(sys.cpu.ready == 1); // Cancelled guest workers can reach kernel teardown.
    th.reset();
}
void stop_before_launch(int count)
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess proc;
    Kernel::KProcess *pointer = &proc;
    settings.running = true;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    th->Stop();
    assert(!th->BeginProcessLaunch());
    wait_until([&] { return sys.shutdowns.load() == 1; });
    assert(!proc.closed_while_launching && proc.closes == 1 && sys.runs == 0);
    assert(sys.cpu.ready == 1);
    th.reset();
}
void success(int count)
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess *pointer = nullptr;
    settings.running = true;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    assert(th->BeginProcessLaunch());
    video.p.set_value(true);
    th->ProcessLaunchCompleted(true);
    wait_until([&] { return sys.runs.load() > 0; });
    th->Stop();
    th.reset();
    assert(sys.shutdowns == 1);
}
void failure(int count)
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess *pointer = nullptr;
    settings.running = true;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    assert(th->BeginProcessLaunch());
    th->ProcessLaunchCompleted(false);
    wait_until([&] { return sys.shutdowns.load() == 1; });
    th.reset();
    assert(sys.runs == 0);
}
void pause_while_failed(int count)
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess *pointer = nullptr;
    settings.running = true;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    auto paused = std::async(std::launch::async, [&] { th->SetRunning(false); });
    video.p.set_value(false);
    assert(th->BeginProcessLaunch());
    th->ProcessLaunchCompleted(true);
    if (paused.wait_for(2s) != std::future_status::ready)
        std::abort();
    paused.get();
    th->Stop();
    th.reset();
    assert(sys.shutdowns == 1);
}
void concurrent_pause()
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess *pointer = nullptr;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    auto first = std::async(std::launch::async, [&] { th->SetRunning(false); });
    auto second = std::async(std::launch::async, [&] { th->SetRunning(false); });
    assert(th->BeginProcessLaunch());
    video.p.set_value(true);
    th->ProcessLaunchCompleted(true);
    assert(first.wait_for(2s) == std::future_status::ready);
    assert(second.wait_for(2s) == std::future_status::ready);
    first.get();
    second.get();
    th->Stop();
    th.reset();
    assert(sys.shutdowns == 1);
}
void cancel_during_gpu_load()
{
    Video video;
    Core::System sys;
    sys.mods.video = &video;
    Kernel::KProcess proc;
    Kernel::KProcess * pointer = &proc;
    auto th = std::make_unique<EmuThread>(sys, pointer);
    th->Start();
    assert(th->BeginProcessLaunch());
    th->ProcessLaunchCompleted(true);
    wait_until([&] { return video.waits.load() == 1; });
    th->Stop();
    video.p.set_value(false);
    wait_until([&] { return sys.shutdowns.load() == 1; });
    th.reset();
    assert(sys.cpu.ready == 1 && sys.runs == 0 && proc.closes == 1);
}

int main()
{
    for (int i = 0; i < 120; i++)
    {
        stop_during_active_process(i);
        stop_before_launch(i);
        success(i);
        failure(i);
        pause_while_failed(i);
        concurrent_pause();
        cancel_during_gpu_load();
    }
    std::cout << "PASS 840 production EmuThread scenarios\n";
}
