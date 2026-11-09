#include "nxemu/ui/views/loading_time_estimate.h"
#include <stdexcept>

void Check(bool value)
{
    if (!value) throw std::runtime_error("Loading time estimate regression");
}

int main()
{
    using namespace std::chrono;
    const auto start = LoadingTimeEstimate::Clock::time_point{};
    LoadingTimeEstimate estimate;
    Check(!estimate.Update(5, 0, 100, start));
    Check(!estimate.Update(5, 4, 100, start + seconds(2)));
    const auto steady = estimate.Update(5, 10, 100, start + seconds(5));
    Check(steady && steady->count() == 45);
    Check(!estimate.Update(5, 10, 100, start + seconds(16)));
    const auto resumed = estimate.Update(5, 12, 100, start + seconds(17));
    Check(resumed && resumed->count() > steady->count());
    Check(!estimate.Update(5, 12, 200, start + seconds(18)));
    Check(!estimate.Update(5, 1, 200, start + seconds(19)));
    Check(!estimate.Update(1, 30, 200, start + seconds(20)));
    estimate.Reset();
    // Cached work completed before the phase is first displayed must not inflate the rate.
    Check(!estimate.Update(5, 80, 100, start));
    const auto cached = estimate.Update(5, 90, 100, start + seconds(5));
    Check(cached && cached->count() == 5);
    Check(!estimate.Update(5, 100, 100, start + seconds(10)));
    Check(!estimate.Update(5, 101, 100, start + seconds(11)));
    Check(!estimate.Update(5, 0, 0, start + seconds(12)));
}
