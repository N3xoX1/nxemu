#include <cstdlib>
#include <limits>
#include "yuzu_common/x64/rdtsc.h"

int main() {
    using Common::X64::detail::CalculateRDTSCFrequency;
    const auto check = [](bool condition) { if (!condition) std::abort(); };
    check(CalculateRDTSCFrequency(400'000'000, 100'000'000) == 4'000'000'000ULL);
    check(CalculateRDTSCFrequency(400'000'000, 0) == 0);
    check(CalculateRDTSCFrequency(400'000'000, -1) == 0);
    check(CalculateRDTSCFrequency(400'000'000, 1) == 0);
    check(CalculateRDTSCFrequency(400'000'000, 11'000'000'000LL) == 0);
    check(CalculateRDTSCFrequency(0, 100'000'000) == 0);
    check(CalculateRDTSCFrequency(std::numeric_limits<u64>::max(), 1) == 0);
    check(CalculateRDTSCFrequency(400'005'000, 100'000'000) == 4'000'100'000ULL);
    using Common::X64::detail::MedianRDTSCFrequency;
    check(MedianRDTSCFrequency({4'000'000'000, 4'400'000'000, 3'999'900'000}) == 4'000'000'000);
    check(MedianRDTSCFrequency({0, 4'000'000'000, 4'400'000'000}) == 4'000'000'000);
    check(MedianRDTSCFrequency({0, 0, 4'400'000'000}) == 4'400'000'000);
    check(MedianRDTSCFrequency({0, 0, 0}) == 0);
    check(!Common::X64::IsValidTSCFrequency(0));
    check(!Common::X64::IsValidTSCFrequency(9'000'000'000'000'000'000ULL));
}
