#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>

class LaunchGate
{
public:
    typedef bool (*PrepareCallback)(void * userData);

    enum class Result
    {
        Released,
        Stopped,
        Failed,
    };

    void Arm(PrepareCallback prepare, void * userData)
    {
        m_prepare = prepare;
        m_userData = userData;
        m_awaitingRelease.store(true, std::memory_order_release);
    }

    bool Armed() const
    {
        return m_prepare != nullptr;
    }

    bool AwaitingRelease() const
    {
        return m_awaitingRelease.load(std::memory_order_acquire);
    }

    Result Prepare(std::stop_token stopToken)
    {
        Result result = Result::Failed;
        if (m_prepare(m_userData))
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_releasedCv.wait(lock, stopToken, [this] { return m_released; });
            result = m_released ? Result::Released : Result::Stopped;
        }
        m_awaitingRelease.store(false, std::memory_order_release);
        return result;
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_releasedCv.notify_one();
    }

private:
    PrepareCallback m_prepare = nullptr;
    void * m_userData = nullptr;
    std::mutex m_mutex;
    std::condition_variable_any m_releasedCv;
    bool m_released = false;
    std::atomic<bool> m_awaitingRelease{false};
};
