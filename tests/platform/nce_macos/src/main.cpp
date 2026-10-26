// NCE macOS validation suite. Usage:
//   nce_macos_validate [all|memory|arm64|transitions]
// Set NCEV_TSV=<file> to append every check as a TSV line.
#include <cstdio>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "nce_runtime.h"
#include "probes.h"
#include "report.h"
#include "suite.h"

extern char ** environ;

namespace ncev {

StartupInfo g_startup;

std::vector<HostRegion> EnumerateRegions(std::uint64_t limit) {
    std::vector<HostRegion> regions;
    mach_vm_address_t address = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info),
                                   &count) != KERN_SUCCESS) {
            break;
        }
        if (address >= limit) {
            break;
        }
        regions.push_back({address, address + size, info.user_tag, info.protection});
        address += size;
    }
    return regions;
}

std::string RunChild(const std::string & path, const std::vector<std::string> & args, int & status, short spawn_flags,
                     int * spawn_error) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        status = -1;
        return {};
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (spawn_flags != 0) {
        posix_spawnattr_setflags(&attr, spawn_flags);
    }
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const std::string & a : args) {
        argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int err = posix_spawn(&pid, path.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (spawn_error != nullptr) {
        *spawn_error = err;
    }
    std::string out;
    if (err == 0) {
        char buffer[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            out.append(buffer, static_cast<size_t>(n));
        }
        waitpid(pid, &status, 0);
    } else {
        status = -1;
    }
    close(pipefd[0]);
    return out;
}

namespace {

bool Reserve(std::uint64_t lo, std::uint64_t hi, kern_return_t * out_kr = nullptr) {
    mach_vm_address_t address = lo;
    const kern_return_t kr = mach_vm_map(mach_task_self(), &address, hi - lo, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE,
                                         VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
    if (out_kr != nullptr) {
        *out_kr = kr;
    }
    return kr == KERN_SUCCESS && address == lo;
}

// Reserves the guest windows with no access before the suite allocates anything.
void ReserveGuestWindows() {
    g_startup.regions_before = EnumerateRegions(GuestSpaceEnd);
    kern_return_t kr = KERN_SUCCESS;
    g_startup.reserved = Reserve(HighWindowLo, HighWindowHi, &kr);
    g_startup.reserve_kr = kr;
    // Every free hole of at least 16 MiB between 4 GiB and 63 GiB.
    std::uint64_t cursor = 0x1'0000'0000ULL;
    auto take = [&](std::uint64_t end) {
        end = std::min(end, LowWindowLimit);
        if (end > cursor && end - cursor >= (16ULL << 20) && Reserve(cursor, end)) {
            g_startup.low_windows.push_back({cursor, end});
        }
    };
    for (const HostRegion & r : g_startup.regions_before) {
        if (r.start > cursor) {
            take(r.start);
        }
        cursor = std::max(cursor, r.end);
        if (cursor >= LowWindowLimit) {
            break;
        }
    }
    take(LowWindowLimit);
}

std::string SelfPath() {
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
        return {};
    }
    char resolved[PATH_MAX];
    return realpath(path, resolved) ? resolved : path;
}

} // namespace

} // namespace ncev

int main(int argc, char ** argv) {
    using namespace ncev;
    if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
        return RunChildMode(argv[2]);
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    ReserveGuestWindows();
    g_startup.self_path = SelfPath();
    g_startup.probe_dir = g_startup.self_path.substr(0, g_startup.self_path.rfind('/'));

    InstallProbeHandler();
    if (g_startup.reserved) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> windows = g_startup.low_windows;
        windows.push_back({HighWindowLo, HighWindowHi});
        RuntimeInit(windows);
    }

    const std::string mode = argc >= 2 ? argv[1] : "all";
    std::printf("NCE macOS validation — %s\n", mode.c_str());
    if (mode == "all" || mode == "memory") {
        RunMemoryTests();
    }
    if (!g_startup.reserved) {
        std::printf("Guest window not reserved: steps 3 and 4 cannot run.\n");
        WriteTsv();
        return 2;
    }
    if (mode == "all" || mode == "arm64") {
        RunArm64Tests();
    }
    if (mode == "all" || mode == "store-thunks") { RunStoreThunkTests(); }
    if (mode == "all" || mode == "read-thunks") {
        RunReadThunkTests();
    }
    if (mode == "all" || mode == "transitions") {
        RunTransitionTests();
    }
    WriteTsv();
    return ExitCode();
}
