#include "process_usage.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <unistd.h>
#endif
#endif

uint64_t ProcessMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS ? info.phys_footprint : 0;
#else
    // The second number of the file is the pages that are in memory.
    unsigned long long total = 0, resident = 0;
    std::FILE * file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr)
    {
        return 0;
    }
    const bool read = std::fscanf(file, "%llu %llu", &total, &resident) == 2;
    std::fclose(file);
    return read ? resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) : 0;
#endif
}

double ProcessCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        return 0.0;
    }
    const auto ticks = [](const FILETIME & time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7; // a tick is 100 ns
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return 0.0;
    }
    const auto seconds = [](const timeval & time) { return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}
