// SPDX-License-Identifier: GPL-2.0-or-later
#include "nxemu-os/launch_gate.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace
{
void Check(bool value, const char * what)
{
    if (!value)
    {
        throw std::runtime_error(what);
    }
}

// Stands in for module initialization and PrepareRom: work split into steps that check a
// cancel flag between them, as the loader does between NSO modules.
struct Preparation
{
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};
    std::chrono::milliseconds step{0};
    int steps = 0;
    bool succeed = true;

    static bool Run(void * userData)
    {
        Preparation & preparation = *static_cast<Preparation *>(userData);
        preparation.running = true;
        for (int i = 0; i < preparation.steps; i++)
        {
            if (preparation.cancelled)
            {
                return false;
            }
            std::this_thread::sleep_for(preparation.step);
        }
        return preparation.succeed && !preparation.cancelled;
    }
};

// Mirrors the emulation thread: prepare, then wait to be released or stopped.
struct Launch
{
    LaunchGate gate;
    Preparation preparation;
    std::promise<LaunchGate::Result> result;
    std::future<LaunchGate::Result> done = result.get_future();
    std::jthread thread;

    void Start()
    {
        gate.Arm(Preparation::Run, &preparation);
        thread = std::jthread([this](std::stop_token token) { result.set_value(gate.Prepare(token)); });
    }

    void Cancel()
    {
        preparation.cancelled = true;
        thread.request_stop();
    }

    LaunchGate::Result Wait(std::chrono::milliseconds limit)
    {
        Check(done.wait_for(limit) == std::future_status::ready, "launch did not finish in time");
        return done.get();
    }
};

void WaitRunning(Launch & launch)
{
    for (int i = 0; i < 200 && !launch.preparation.running; i++)
    {
        std::this_thread::sleep_for(1ms);
    }
    Check(launch.preparation.running, "preparation never started");
}

void NormalLaunch()
{
    Launch launch;
    launch.Start();
    Check(launch.gate.AwaitingRelease(), "armed gate must await release");
    Check(launch.done.wait_for(40ms) == std::future_status::timeout, "prepared launch must wait to be released");
    launch.gate.Release();
    Check(launch.Wait(1s) == LaunchGate::Result::Released, "released launch must start");
    Check(!launch.gate.AwaitingRelease(), "started launch no longer awaits release");
}

void ReleaseBeforePrepared()
{
    Launch launch;
    launch.preparation.steps = 5;
    launch.preparation.step = 5ms;
    launch.Start();
    launch.gate.Release();
    Check(launch.Wait(1s) == LaunchGate::Result::Released, "early release must not be lost");
}

void FailedInitialization()
{
    Launch launch;
    launch.preparation.succeed = false;
    launch.Start();
    Check(launch.Wait(1s) == LaunchGate::Result::Failed, "failed preparation must not wait for release");
    Check(!launch.gate.AwaitingRelease(), "failed launch no longer awaits release");
}

void CancelDuringPreparation()
{
    Launch launch;
    launch.preparation.steps = 100;
    launch.preparation.step = 10ms;
    launch.Start();
    WaitRunning(launch);
    const std::chrono::steady_clock::time_point cancelled = std::chrono::steady_clock::now();
    launch.Cancel();
    Check(launch.Wait(1s) == LaunchGate::Result::Failed, "cancelled preparation must not start");
    Check(std::chrono::steady_clock::now() - cancelled < 200ms, "cancellation must stop at the next step, not finish loading");
}

void CancelAfterPreparation()
{
    Launch launch;
    launch.Start();
    Check(launch.done.wait_for(20ms) == std::future_status::timeout, "prepared launch must wait to be released");
    launch.Cancel();
    Check(launch.Wait(1s) == LaunchGate::Result::Stopped, "stop while waiting must not start");
}

void ShutdownDuringLoading()
{
    std::chrono::steady_clock::time_point shutdown;
    {
        Launch launch;
        launch.preparation.steps = 100;
        launch.preparation.step = 10ms;
        launch.Start();
        WaitRunning(launch);
        shutdown = std::chrono::steady_clock::now();
        launch.preparation.cancelled = true;
    }
    Check(std::chrono::steady_clock::now() - shutdown < 200ms, "shutdown during loading must not wait for loading to finish");
}

void RepeatedStartStopRestart()
{
    int released = 0;
    int stopped = 0;
    int failed = 0;
    for (int i = 0; i < 300; i++)
    {
        Launch launch;
        launch.preparation.steps = i % 3;
        launch.preparation.step = 1ms;
        launch.preparation.succeed = i % 7 != 0;
        launch.Start();
        switch (i % 4)
        {
        case 0: launch.gate.Release(); break;
        case 1: launch.Cancel(); break;
        case 2: std::this_thread::sleep_for(1ms); launch.Cancel(); break;
        default: launch.gate.Release(); launch.Cancel(); break;
        }
        const LaunchGate::Result result = launch.Wait(2s);
        released += result == LaunchGate::Result::Released;
        stopped += result == LaunchGate::Result::Stopped;
        failed += result == LaunchGate::Result::Failed;
        Check(!launch.gate.AwaitingRelease(), "finished launch must not await release");
    }
    Check(released > 0 && failed > 0 && released + stopped + failed == 300, "every cycle must finish with one result");
}
} // namespace

int main()
try
{
    NormalLaunch();
    ReleaseBeforePrepared();
    FailedInitialization();
    CancelDuringPreparation();
    CancelAfterPreparation();
    ShutdownDuringLoading();
    RepeatedStartStopRestart();
    std::cout << "PASS: launch release, failed preparation, cancellation, shutdown during loading, 300 start/stop cycles\n";
    return 0;
}
catch (const std::exception & error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
