#include <thread>
#include <yuzu_common/thread.h>
#include <yuzu_common/startup_wait.h>
#include <yuzu_common/logging/log.h>
#include <nxemu-core/settings/identifiers.h>
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
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
        Common::SetCurrentThreadName("EmuControlThread");

        auto stop_token = m_stop_source.get_token();
        if (m_prepare != nullptr)
        {
            Prepare(stop_token);
        }
        if (!m_system.IsPoweredOn())
        {
            g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
            return;
        }
        m_system.RegisterHostThread();

        // Host HLE service threads must initialize while shaders load, including
        // when startup is cancelled. Suspend only the application before opening
        // the CPU barrier; System::Run resumes it once GPU preparation is done.
        if (m_process != nullptr)
        {
            Kernel::KScopedLightLock lock{m_process->GetListLock()};
            for (auto& thread : m_process->GetThreadList())
            {
                thread.RequestSuspend(Kernel::SuspendType::System);
            }
        }
        m_system.GetCpuManager().OnGpuReady();
        const auto gpu_state = [] {
            return static_cast<GpuInitializationState>(g_settings->GetInt(NXCoreSetting::GpuInitializationState));
        };
        const bool gpu_settled = Common::WaitForStartup(stop_token, [&] {
            return gpu_state() != GpuInitializationState::Preparing;
        });
        const bool gpu_ready = gpu_settled && gpu_state() == GpuInitializationState::Ready;
        if (gpu_settled && !gpu_ready)
        {
            LOG_ERROR(Core, "GPU initialization failed; guest execution will not start");
        }
        if (gpu_ready)
        {
            LOG_INFO(Core, "GPU preparation finished; starting guest execution");
            m_started.store(true, std::memory_order_release);
            g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Running);
        }

        while (gpu_ready && !stop_token.stop_requested())
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
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
    }

    void Prepare(std::stop_token stop_token)
    {
        const bool prepared = m_prepare(m_prepareUserData);
        std::unique_lock lk{m_should_run_mutex};
        if (prepared)
        {
            Common::CondvarWait(m_should_run_cv, lk, stop_token, [&] {
                return m_released;
            });
        }
        else
        {
            m_stop_source.request_stop();
        }
        m_awaitingRelease.store(false, std::memory_order_release);
    }

    std::thread m_thread;
    std::stop_source m_stop_source;
    std::mutex m_should_run_mutex;
    std::condition_variable_any m_should_run_cv;
    Common::Event m_stopped;
    std::atomic<bool> m_should_run;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_awaitingRelease{false};
    EmulationPrepareCallback m_prepare = nullptr;
    void * m_prepareUserData = nullptr;
    bool m_released = false;
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

void EmuThread::StartPrepared(EmulationPrepareCallback prepare, void * userData)
{
    impl->m_prepare = prepare;
    impl->m_prepareUserData = userData;
    impl->m_awaitingRelease.store(true, std::memory_order_release);
    Start();
}

bool EmuThread::AwaitingRelease() const
{
    return impl->m_awaitingRelease.load(std::memory_order_acquire);
}

void EmuThread::Release()
{
    std::lock_guard<std::mutex> lk{impl->m_should_run_mutex};
    impl->m_released = true;
    impl->m_should_run_cv.notify_one();
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

    // Before GPU readiness no guest code has run, so there is nothing to pause.
    // Waiting for the normal pause acknowledgement here would block startup UI.
    if (!should_run && impl->m_started.load(std::memory_order_acquire))
    {
        impl->m_stopped.Wait();
    }
}

bool EmuThread::IsRunning() const
{
    return impl->m_should_run.load(std::memory_order_acquire);
}
