// Shared state of the validation suite.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ncev {

// Identity mapped guest addresses must stay below 512 GiB (39-bit Horizon address
// space). On Darwin arm64 the kernel reserves [63 GiB, 448 GiB) permanently, so the
// NCE windows are [448 GiB, 512 GiB) and the free holes between 4 GiB and 63 GiB,
// all reserved at startup.
constexpr std::uint64_t GuestSpaceEnd = 0x80'0000'0000ULL;   // 512 GiB
constexpr std::uint64_t HighWindowLo = 0x70'0000'0000ULL;    // 448 GiB
constexpr std::uint64_t HighWindowHi = GuestSpaceEnd;
constexpr std::uint64_t LowWindowLimit = 0xF'C000'0000ULL;   // 63 GiB
constexpr std::uint64_t CarveoutLo = LowWindowLimit;
constexpr std::uint64_t CarveoutHi = HighWindowLo;

struct HostRegion {
    std::uint64_t start;
    std::uint64_t end;
    std::uint32_t tag;
    int prot;
};

struct StartupInfo {
    std::vector<HostRegion> regions_before; // host map before the reservation
    bool reserved = false;                  // high window reserved
    int reserve_kr = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> low_windows; // reserved low holes
    std::string self_path;
    std::string probe_dir;
};
extern StartupInfo g_startup;

std::vector<HostRegion> EnumerateRegions(std::uint64_t limit);

// Runs this executable with "--child <mode>" and returns its stdout. status receives
// the waitpid status. spawn_flags are posix_spawnattr flags.
std::string RunChild(const std::string & path, const std::vector<std::string> & args, int & status,
                     short spawn_flags = 0, int * spawn_error = nullptr);

void RunMemoryTests();
void RunArm64Tests();
void RunTransitionTests();
void RunReadThunkTests();
void RunStoreThunkTests();

int RunChildMode(const std::string & mode);

// SDK version the validation executable declares (tests/platform/CMakeLists.txt). XNU keeps x18 for every
// thread of a process whose main executable declares an SDK older than 13.0.
#ifndef NCEV_LEGACY_SDK
#error "tests/platform/CMakeLists.txt defines NCEV_LEGACY_SDK"
#endif

// How often a value written to x18 survives the kernel (A3.1, A3.2).
struct X18Survival {
    std::uint64_t spin_lost = 0, spin_total = 0, spin_seen = 0;
    std::uint64_t sys_lost = 0, sig_lost = 0, mcontext_lost = 0;
    int tpidr_flag = 0; // TPIDR_EL0 bit 48, MACHDEP_TPIDR_FLAG_PRESERVE_X18
    bool Preserved() const {
        return spin_lost == 0 && sys_lost == 0 && sig_lost == 0 && mcontext_lost == 0;
    }
};
X18Survival MeasureX18();
std::string X18Text(const X18Survival & r);

bool MetalSmoke(std::string & detail);

} // namespace ncev
