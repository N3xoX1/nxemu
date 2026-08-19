#include <thread>
#include <yuzu_common/thread.h>
#include <nxemu-core/settings/identifiers.h>
#include "core/hle/kernel/k_process.h"
#include "core/cpu_manager.h"
#include "core/core.h"
#include "emu_thread.h"

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

        m_system.GetCpuManager().OnGpuReady();
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Running);

        while (!stop_token.stop_requested())
        {
            std::unique_lock lk{m_should_run_mutex};
            if (m_should_run.load(std::memory_order_relaxed))
            {
                m_system.Run();
                m_stopped.Reset();

                Common::CondvarWait(m_should_run_cv, lk, stop_token, [&] {
                    return !m_should_run.load(std::memory_order_relaxed);
                });
            }
            else
            {
                m_system.Pause();
                m_stopped.Set();
                g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Paused);

                Common::CondvarWait(m_should_run_cv, lk, stop_token, [&] {
                    return m_should_run.load(std::memory_order_relaxed);
                });

                if (!stop_token.stop_requested())
                {
                    g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Running);
                }
            }
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
    std::mutex m_should_run_mutex;
    std::condition_variable_any m_should_run_cv;
    Common::Event m_stopped;
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

void EmuThread::Stop()
{
    impl->m_stop_source.request_stop();
    {
        std::lock_guard<std::mutex> lk{impl->m_should_run_mutex};
        impl->m_should_run.store(false, std::memory_order_release);
        impl->m_should_run_cv.notify_one();
    }
}

void EmuThread::SetRunning(bool should_run)
{
    // TODO: Prevent other threads from modifying the state until we finish.
    {
        // Notify the running thread to change state.
        std::unique_lock run_lk{impl->m_should_run_mutex};
        impl->m_should_run.store(should_run, std::memory_order_release);
        impl->m_should_run_cv.notify_one();
    }

    // Wait until paused, if pausing.
    if (!should_run)
    {
        impl->m_stopped.Wait();
    }
}

bool EmuThread::IsRunning() const
{
    return impl->m_should_run.load(std::memory_order_acquire);
}
