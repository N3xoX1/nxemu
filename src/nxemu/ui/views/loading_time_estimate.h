#pragma once
#include <chrono>
#include <cmath>
#include <optional>

// Estimate the current measured phase only. Different shaders have different
// costs, so smooth the measured rate and hide stale estimates during long builds.
class LoadingTimeEstimate
{
public:
    using Clock = std::chrono::steady_clock;

    void Reset() { *this = LoadingTimeEstimate{}; }

    std::optional<std::chrono::seconds> Update(int stage, int current, int total,
                                              Clock::time_point now = Clock::now())
    {
        if (total <= 0 || current < 0 || current > total)
        {
            Reset();
            return {};
        }
        if (stage != m_stage || total != m_total || current < m_current)
        {
            m_stage = stage;
            m_total = total;
            m_first = m_sample = m_current = current;
            m_started = m_sampled = m_progressed = now;
            m_rate = 0;
            return {};
        }
        if (current > m_current)
        {
            m_current = current;
            m_progressed = now;
            const double elapsed = std::chrono::duration<double>(now - m_sampled).count();
            if (elapsed >= 1)
            {
                const double rate = (current - m_sample) / elapsed;
                m_rate = m_rate > 0 ? 0.75 * m_rate + 0.25 * rate : rate;
                m_sample = current;
                m_sampled = now;
            }
        }
        if (current == total || current - m_first < 8 || m_rate <= 0 ||
            now - m_started < std::chrono::seconds(5) ||
            now - m_progressed > std::chrono::seconds(10))
            return {};
        return std::chrono::seconds(static_cast<long long>(std::ceil((total - current) / m_rate)));
    }

private:
    int m_stage = -1, m_total = 0, m_first = 0, m_sample = 0, m_current = 0;
    double m_rate = 0;
    Clock::time_point m_started{}, m_sampled{}, m_progressed{};
};
