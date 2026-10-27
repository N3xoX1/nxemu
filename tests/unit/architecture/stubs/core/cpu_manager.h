#pragma once
#include <atomic>
namespace Core
{
struct CpuManager
{
    std::atomic<int> ready{0};
    void OnGpuReady()
    {
        ready++;
    }
};
} // namespace Core
