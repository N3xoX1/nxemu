#include <thread>
#include <yuzu_common/thread.h>
#include <nxemu-core/settings/identifiers.h>
#include "core/hle/kernel/k_process.h"
#include "core/cpu_manager.h"
#include "core/core.h"
#include "emu_thread.h"
#include <nxemu-module-spec/video.h>

extern IModuleSettings * g_settings;

struct EmuThread::Impl
{
public:
    Impl(Core::System & system, Kernel::KProcess *& process) :
        m_system(system),
        m_process(process),
        m_should_run(true)
    {
    }

    void Run()
    {
        const char * name = "EmuControlThread";
#if MICROPROFILE_ENABLED
        MicroProfileOnThreadCreate(name);
#endif
        Common::SetCurrentThreadName(name);

        auto stop_token = m_stop_source.get_token();
        m_system.RegisterHostThread();

        // The loader owns the process until it finishes launching it. Stop()
        // cancels guest execution, but must never cancel the ownership gate:
        // an in-progress launch must finish before we can close KProcess.
        bool launched = false;
        {
            std::unique_lock lk{m_launch_mutex};
            m_launch_cv.wait(lk, [&] { return m_launch_finished; });
            launched = m_launch_finished && m_launch_succeeded;
        }

        // Shader-cache loading remains asynchronous to the frontend. Guest
        // execution begins only after process launch AND GPU startup succeed.
        const bool gpu_ready = launched && !stop_token.stop_requested() &&
            m_system.GetSystemModules().Video().WaitForGuestGpu() && !stop_token.stop_requested();
        if (gpu_ready || m_process != nullptr)
        {
            // SetupCurrentProcess already created CPU workers waiting at this
            // rendezvous. They must enter the kernel even on cancelled startup
            // so ShutdownMainProcess can schedule its termination threads.
            // Otherwise it waits for workers still blocked behind the GPU gate.
            m_system.GetCpuManager().OnGpuReady();
        }
        if (gpu_ready && !stop_token.stop_requested())
        {
            g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Running);

            while (!stop_token.stop_requested())
            {
                std::unique_lock lk{m_should_run_mutex};
                if (m_should_run.load(std::memory_order_relaxed))
                {
                    m_system.Run();
                    m_pause_acknowledged = false;

                    Common::CondvarWait(m_should_run_cv, lk, stop_token, [&] {
                        return !m_should_run.load(std::memory_order_relaxed);
                    });
                }
                else
                {
                    m_system.Pause();
                    m_pause_acknowledged = true;
                    m_paused_cv.notify_all();
                    g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Paused);

                    Common::CondvarWait(m_should_run_cv, lk, stop_token, [&] {
                        return m_should_run.load(std::memory_order_relaxed);
                    });

                    if (!stop_token.stop_requested())
                    {
                        m_pause_acknowledged = false;
                        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Running);
                    }
                }
            }
        }
        else
        {
            // Mark aborted startup before unblocking any pause waiters.
            // Later pause requests must also return without waiting for a
            // control thread that is already exiting.
            {
                std::lock_guard lk{m_should_run_mutex};
                m_stop_source.request_stop();
            }
            m_paused_cv.notify_all();
            // Failure (or shutdown during startup): use the existing stop
            // path. StopEmulation(false) is safe from this OS thread.
            g_settings->SetBool(NXCoreSetting::EmulationRunning, false);
        }

        // Shutdown the main emulated process
        if (m_process != nullptr)
        {
            m_process->Close();
            m_process = nullptr;
        }
        m_system.ShutdownMainProcess();

#if MICROPROFILE_ENABLED
        MicroProfileOnThreadExit();
#endif
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
    }

    std::thread m_thread;
    std::stop_source m_stop_source;
    std::mutex m_launch_mutex;
    std::condition_variable_any m_launch_cv;
    bool m_launch_finished = false;
    bool m_launch_succeeded = false;
    bool m_launch_in_progress = false;
    std::mutex m_should_run_mutex;
    std::condition_variable_any m_should_run_cv;
    std::condition_variable m_paused_cv;
    bool m_pause_acknowledged = false; // guarded by m_should_run_mutex
    std::atomic<bool> m_should_run;
    Core::System & m_system;
    Kernel::KProcess *& m_process;
};

EmuThread::EmuThread(Core::System & system, Kernel::KProcess *& process) :
    impl(std::make_unique<Impl>(system, process))
{
}

EmuThread::~EmuThread()
{
    Stop();
    if (impl->m_thread.joinable())
    {
        impl->m_thread.join();
    }
}

void EmuThread::Start()
{
    impl->m_thread = std::thread(&Impl::Run, impl.get());
}

bool EmuThread::BeginProcessLaunch()
{
    std::lock_guard lk{impl->m_launch_mutex};
    // A concurrent Stop() can win before the process launch begins. In that
    // case the loader must not touch a process which is being shut down.
    if (impl->m_launch_finished || impl->m_launch_in_progress || impl->m_stop_source.stop_requested())
        return false;
    impl->m_launch_in_progress = true;
    return true;
}

void EmuThread::ProcessLaunchCompleted(bool succeeded)
{
    {
        std::lock_guard lk{impl->m_launch_mutex};
        // A stopped, never-started launch must not be revived by a late signal.
        if (impl->m_launch_finished && !impl->m_launch_in_progress)
            return;
        impl->m_launch_in_progress = false;
        impl->m_launch_succeeded = succeeded && !impl->m_stop_source.stop_requested();
        impl->m_launch_finished = true;
    }
    impl->m_launch_cv.notify_all();
}

void EmuThread::Stop()
{
    {
        std::lock_guard lk{impl->m_launch_mutex};
        impl->m_stop_source.request_stop();
        // If the loader has already entered StartApplicationProcess(), its
        // completion is mandatory even during cancellation. Otherwise the
        // stopped guest may release its process immediately; a late loader
        // cannot enter the launch gate after this point.
        if (!impl->m_launch_in_progress && !impl->m_launch_finished)
        {
            impl->m_launch_finished = true;
            impl->m_launch_succeeded = false;
        }
    }
    impl->m_launch_cv.notify_all();
    {
        std::lock_guard<std::mutex> lk{impl->m_should_run_mutex};
        impl->m_should_run.store(false, std::memory_order_release);
        impl->m_should_run_cv.notify_all();
    }
    // A caller waiting in SetRunning(false) must not remain blocked when
    // the guest is stopped before it reaches its first pause point.
    impl->m_paused_cv.notify_all();
}

void EmuThread::SetRunning(bool should_run)
{
    std::unique_lock run_lk{impl->m_should_run_mutex};
    impl->m_should_run.store(should_run, std::memory_order_release);
    impl->m_should_run_cv.notify_all();
    if (!should_run)
    {
        // A predicate acknowledges all pause callers and cannot consume a
        // stale auto-reset event left by an earlier pause or cancellation.
        impl->m_paused_cv.wait(run_lk, [&] {
            return impl->m_pause_acknowledged || impl->m_stop_source.stop_requested();
        });
    }
}

bool EmuThread::IsRunning() const
{
    return impl->m_should_run.load(std::memory_order_acquire);
}
