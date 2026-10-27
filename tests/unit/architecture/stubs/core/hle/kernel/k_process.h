#pragma once
#include <atomic>
namespace Kernel
{
class KProcess
{
  public:
    std::atomic<int> closes{0};
    std::atomic<bool> launch_active{false}, closed_while_launching{false};
    void Close()
    {
        if (launch_active.load())
            closed_while_launching = true;
        closes++;
    }
};
} // namespace Kernel
