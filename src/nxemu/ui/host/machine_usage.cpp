#include "machine_usage.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cwchar>
#include <cwctype>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <dirent.h>
#include <fstream>
#endif

namespace
{
template <typename T>
bool Symbol(SDL_SharedObject * library, const char * name, T & function)
{
    function = reinterpret_cast<T>(SDL_LoadFunction(library, name));
    return function != nullptr;
}

std::string Trim(std::string text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::string ProcessorName()
{
#if defined(_WIN32)
    char name[128] = {};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
    {
        return Trim(name);
    }
    return std::string();
#elif defined(__APPLE__)
    char name[128] = {};
    size_t size = sizeof(name);
    return sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0 ? Trim(name) : std::string();
#else
    std::ifstream file("/proc/cpuinfo");
    for (std::string line; std::getline(file, line);)
    {
        if (line.compare(0, 10, "model name") == 0 && line.find(':') != std::string::npos)
        {
            return Trim(line.substr(line.find(':') + 1));
        }
    }
    return std::string();
#endif
}

class Nvml
{
public:
    explicit Nvml(const GraphicsIdentity & graphics)
    {
#if defined(_WIN32)
        m_library = SDL_LoadObject("nvml.dll");
        if (m_library == nullptr)
        {
            m_library = SDL_LoadObject("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
#else
        m_library = SDL_LoadObject("libnvidia-ml.so.1");
#endif
        int (*init)() = nullptr;
        int (*count)(unsigned *) = nullptr;
        int (*handle)(unsigned, void **) = nullptr;
        int (*pciInfo)(void *, PciInfo *) = nullptr;
        if (m_library == nullptr || !Symbol(m_library, "nvmlInit_v2", init) || !Symbol(m_library, "nvmlShutdown", m_shutdown) ||
            !Symbol(m_library, "nvmlDeviceGetCount_v2", count) || !Symbol(m_library, "nvmlDeviceGetHandleByIndex_v2", handle) ||
            !Symbol(m_library, "nvmlDeviceGetPciInfo_v3", pciInfo) || init() != 0)
        {
            m_shutdown = nullptr;
            return;
        }
        Symbol(m_library, "nvmlDeviceGetUtilizationRates", m_utilization);
        Symbol(m_library, "nvmlDeviceGetMemoryInfo", m_memory);
        Symbol(m_library, "nvmlDeviceGetTemperature", m_temperature);
        Symbol(m_library, "nvmlDeviceGetPowerUsage", m_power);
        Symbol(m_library, "nvmlDeviceGetClockInfo", m_clock);
        const unsigned wanted = (graphics.deviceId << 16) | graphics.vendorId;
        unsigned devices = 0;
        if (count(&devices) != 0)
        {
            return;
        }
        for (unsigned i = 0; i < devices && m_device == nullptr; i++)
        {
            void * device = nullptr;
            PciInfo info{};
            if (handle(i, &device) == 0 && pciInfo(device, &info) == 0 && info.pciDeviceId == wanted)
            {
                m_device = device;
            }
        }
    }

    ~Nvml()
    {
        if (m_shutdown != nullptr)
        {
            m_shutdown();
        }
        if (m_library != nullptr)
        {
            SDL_UnloadObject(m_library);
        }
    }

    void Sample(MachineUsage & usage) const
    {
        if (m_device == nullptr)
        {
            return;
        }
        Utilization utilization{};
        Memory memory{};
        unsigned value = 0;
        if (usage.gpu < 0.0 && m_utilization != nullptr && m_utilization(m_device, &utilization) == 0)
        {
            usage.gpu = utilization.gpu;
        }
        if (m_memory != nullptr && m_memory(m_device, &memory) == 0)
        {
            usage.videoMemoryUsed = usage.videoMemoryUsed != 0 ? usage.videoMemoryUsed : memory.used;
            usage.videoMemoryTotal = memory.total;
        }
        if (m_temperature != nullptr && m_temperature(m_device, 0, &value) == 0)
        {
            usage.gpuTemperature = value;
        }
        if (m_power != nullptr && m_power(m_device, &value) == 0)
        {
            usage.gpuPower = value / 1000.0;
        }
        if (m_clock != nullptr && m_clock(m_device, 0, &value) == 0)
        {
            usage.gpuClock = value;
        }
    }

private:
    Nvml(const Nvml &) = delete;
    Nvml & operator=(const Nvml &) = delete;

    struct PciInfo
    {
        char busIdLegacy[16];
        unsigned domain, bus, device, pciDeviceId, pciSubSystemId;
        char busId[32];
    };
    struct Utilization
    {
        unsigned gpu, memory;
    };
    struct Memory
    {
        unsigned long long total, free, used;
    };

    SDL_SharedObject * m_library = nullptr;
    void * m_device = nullptr;
    int (*m_shutdown)() = nullptr;
    int (*m_utilization)(void *, Utilization *) = nullptr;
    int (*m_memory)(void *, Memory *) = nullptr;
    int (*m_temperature)(void *, int, unsigned *) = nullptr;
    int (*m_power)(void *, unsigned *) = nullptr;
    int (*m_clock)(void *, int, unsigned *) = nullptr;
};

#if defined(_WIN32)
class Adl
{
public:
    explicit Adl(const GraphicsIdentity & graphics)
    {
        if (graphics.vendorId != 0x1002)
        {
            return;
        }
        m_library = SDL_LoadObject("atiadlxx.dll");
        int (*create)(void * (__stdcall *)(int), int, void **) = nullptr;
        int (*count)(void *, int *) = nullptr;
        int (*info)(void *, AdapterInfo *, int) = nullptr;
        if (m_library == nullptr || !Symbol(m_library, "ADL2_Main_Control_Create", create) || !Symbol(m_library, "ADL2_Main_Control_Destroy", m_destroy) ||
            !Symbol(m_library, "ADL2_Adapter_NumberOfAdapters_Get", count) || !Symbol(m_library, "ADL2_Adapter_AdapterInfo_Get", info) ||
            !Symbol(m_library, "ADL2_New_QueryPMLogData_Get", m_log) || create(Allocate, 1, &m_context) != 0)
        {
            m_context = nullptr;
            return;
        }
        int adapters = 0;
        if (count(m_context, &adapters) != 0 || adapters <= 0 || adapters > 256)
        {
            return;
        }
        std::vector<AdapterInfo> list(static_cast<size_t>(adapters));
        for (AdapterInfo & adapter : list)
        {
            adapter.size = sizeof(AdapterInfo);
        }
        if (info(m_context, list.data(), adapters * static_cast<int>(sizeof(AdapterInfo))) != 0)
        {
            return;
        }
        char device[16];
        std::snprintf(device, sizeof(device), "DEV_%04X", graphics.deviceId);
        for (const AdapterInfo & adapter : list)
        {
            std::string udid = adapter.udid;
            std::transform(udid.begin(), udid.end(), udid.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (adapter.vendorId != 1002 || udid.find(device) == std::string::npos)
            {
                continue;
            }
            m_buffer.size = sizeof(m_buffer);
            if (m_log(m_context, adapter.index, &m_buffer) == 0)
            {
                m_adapter = adapter.index;
                break;
            }
        }
    }

    ~Adl()
    {
        if (m_context != nullptr)
        {
            m_destroy(m_context);
        }
        if (m_library != nullptr)
        {
            SDL_UnloadObject(m_library);
        }
    }

    void Sample(MachineUsage & usage)
    {
        if (m_adapter < 0)
        {
            return;
        }
        m_buffer = {};
        m_buffer.size = sizeof(m_buffer);
        if (m_log(m_context, m_adapter, &m_buffer) != 0)
        {
            return;
        }
        const Sensor * sensors = m_buffer.sensors;
        if (usage.gpu < 0.0 && sensors[kActivity].supported)
        {
            usage.gpu = sensors[kActivity].value;
        }
        if (sensors[kEdgeTemperature].supported)
        {
            usage.gpuTemperature = sensors[kEdgeTemperature].value;
        }
        else if (sensors[kHotspotTemperature].supported)
        {
            usage.gpuTemperature = sensors[kHotspotTemperature].value;
        }
        if (sensors[kBoardPower].supported)
        {
            usage.gpuPower = sensors[kBoardPower].value;
        }
        else if (sensors[kChipPower].supported)
        {
            usage.gpuPower = sensors[kChipPower].value;
        }
        if (sensors[kClock].supported)
        {
            usage.gpuClock = sensors[kClock].value;
        }
    }

private:
    Adl(const Adl &) = delete;
    Adl & operator=(const Adl &) = delete;

    enum
    {
        kClock = 1,
        kEdgeTemperature = 8,
        kActivity = 19,
        kChipPower = 23,
        kHotspotTemperature = 27,
        kBoardPower = 73,
    };
    struct AdapterInfo
    {
        int size, index;
        char udid[256];
        int bus, device, function, vendorId;
        char adapterName[256], displayName[256];
        int present, exist;
        char driverPath[256], driverPathExt[256], pnpString[256];
        int osDisplayIndex;
    };
    struct Sensor
    {
        int supported, value;
    };
    struct Log
    {
        int size;
        Sensor sensors[256];
    };

    static void * __stdcall Allocate(int size)
    {
        return std::malloc(static_cast<size_t>(size));
    }

    SDL_SharedObject * m_library = nullptr;
    void * m_context = nullptr;
    int m_adapter = -1;
    int (*m_destroy)(void *) = nullptr;
    int (*m_log)(void *, int, Log *) = nullptr;
    Log m_buffer{};
};

class SystemCounters
{
public:
    SystemCounters(const GraphicsIdentity & graphics, bool engines)
    {
        if (graphics.hasLuid)
        {
            uint32_t low, high;
            std::memcpy(&low, graphics.luid, sizeof(low));
            std::memcpy(&high, graphics.luid + 4, sizeof(high));
            wchar_t luid[40];
            swprintf(luid, sizeof(luid) / sizeof(luid[0]), L"luid_0x%08x_0x%08x", high, low);
            m_luid = luid;
        }
        m_library = SDL_LoadObject("pdh.dll");
        PDH_STATUS (WINAPI * open)(LPCWSTR, DWORD_PTR, PDH_HQUERY *) = nullptr;
        PDH_STATUS (WINAPI * add)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *) = nullptr;
        if (m_library == nullptr || !Symbol(m_library, "PdhOpenQueryW", open) || !Symbol(m_library, "PdhAddEnglishCounterW", add) ||
            !Symbol(m_library, "PdhCollectQueryData", m_collect) || !Symbol(m_library, "PdhGetFormattedCounterArrayW", m_values) ||
            !Symbol(m_library, "PdhCloseQuery", m_close) || open(nullptr, 0, &m_query) != ERROR_SUCCESS)
        {
            m_query = nullptr;
            return;
        }
        if (!engines || add(m_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_engines) != ERROR_SUCCESS)
        {
            m_engines = nullptr;
        }
        if (add(m_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &m_memory) != ERROR_SUCCESS)
        {
            m_memory = nullptr;
        }
        m_collect(m_query);
        GetSystemTimes(&m_idle, &m_kernel, &m_user);
    }

    ~SystemCounters()
    {
        if (m_query != nullptr)
        {
            m_close(m_query);
        }
        if (m_library != nullptr)
        {
            SDL_UnloadObject(m_library);
        }
    }

    void Sample(MachineUsage & usage)
    {
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            const uint64_t idleTime = Ticks(idle) - Ticks(m_idle);
            const uint64_t total = Ticks(kernel) - Ticks(m_kernel) + Ticks(user) - Ticks(m_user);
            usage.cpu = total != 0 ? 100.0 * static_cast<double>(total - std::min(idleTime, total)) / static_cast<double>(total) : -1.0;
            m_idle = idle;
            m_kernel = kernel;
            m_user = user;
        }
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory))
        {
            usage.memoryTotal = memory.ullTotalPhys;
            usage.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        }
        if (m_query == nullptr || m_collect(m_query) != ERROR_SUCCESS)
        {
            return;
        }
        std::map<std::wstring, double> engines;
        for (const PDH_FMT_COUNTERVALUE_ITEM_W & item : Values(m_engines))
        {
            const wchar_t * type = wcsstr(item.szName, L"engtype_");
            if (Matches(item.szName))
            {
                engines[type != nullptr ? type : L""] += item.FmtValue.doubleValue;
            }
        }
        for (const auto & engine : engines)
        {
            usage.gpu = std::max(usage.gpu, std::min(engine.second, 100.0));
        }
        double used = 0.0;
        for (const PDH_FMT_COUNTERVALUE_ITEM_W & item : Values(m_memory))
        {
            used = Matches(item.szName) ? used + item.FmtValue.doubleValue : used;
        }
        usage.videoMemoryUsed = static_cast<uint64_t>(used);
    }

private:
    SystemCounters(const SystemCounters &) = delete;
    SystemCounters & operator=(const SystemCounters &) = delete;

    static uint64_t Ticks(const FILETIME & time)
    {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    bool Matches(const wchar_t * name) const
    {
        if (m_luid.empty())
        {
            return false;
        }
        std::wstring lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return lower.find(m_luid) != std::wstring::npos;
    }

    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> Values(PDH_HCOUNTER counter)
    {
        std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items;
        DWORD size = 0, count = 0;
        if (counter == nullptr || m_values(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA || size == 0)
        {
            return items;
        }
        m_buffer.resize(size);
        auto * values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(m_buffer.data());
        if (m_values(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, values) != ERROR_SUCCESS)
        {
            return items;
        }
        for (DWORD i = 0; i < count; i++)
        {
            if (values[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
            {
                items.push_back(values[i]);
            }
        }
        return items;
    }

    std::wstring m_luid;
    SDL_SharedObject * m_library = nullptr;
    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_engines = nullptr;
    PDH_HCOUNTER m_memory = nullptr;
    PDH_STATUS (WINAPI * m_collect)(PDH_HQUERY) = nullptr;
    PDH_STATUS (WINAPI * m_values)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W) = nullptr;
    PDH_STATUS (WINAPI * m_close)(PDH_HQUERY) = nullptr;
    std::vector<uint8_t> m_buffer;
    FILETIME m_idle{}, m_kernel{}, m_user{};
};
#elif defined(__APPLE__)
class SystemCounters
{
public:
    explicit SystemCounters(const GraphicsIdentity &)
    {
        Times(m_busy, m_total);
    }

    void Sample(MachineUsage & usage)
    {
        uint64_t busy = 0, total = 0;
        if (Times(busy, total) && total > m_total)
        {
            usage.cpu = 100.0 * static_cast<double>(busy - m_busy) / static_cast<double>(total - m_total);
            m_busy = busy;
            m_total = total;
        }
        uint64_t memory = 0;
        size_t size = sizeof(memory);
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_size_t page = 0;
        if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0 && host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
            host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
        {
            usage.memoryTotal = memory;
            usage.memoryUsed = static_cast<uint64_t>(vm.active_count + vm.wire_count + vm.compressor_page_count) * page;
        }
    }

private:
    static bool Times(uint64_t & busy, uint64_t & total)
    {
        host_cpu_load_info_data_t load{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&load), &count) != KERN_SUCCESS)
        {
            return false;
        }
        busy = load.cpu_ticks[CPU_STATE_USER] + load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_NICE];
        total = busy + load.cpu_ticks[CPU_STATE_IDLE];
        return true;
    }

    uint64_t m_busy = 0, m_total = 0;
};
#else
class SystemCounters
{
public:
    explicit SystemCounters(const GraphicsIdentity & graphics)
    {
        Times(m_busy, m_total);
        DIR * cards = opendir("/sys/class/drm");
        for (dirent * entry = cards != nullptr ? readdir(cards) : nullptr; entry != nullptr && m_device.empty(); entry = readdir(cards))
        {
            const std::string name = entry->d_name;
            if (name.compare(0, 4, "card") != 0 || name.find('-') != std::string::npos)
            {
                continue;
            }
            const std::string device = "/sys/class/drm/" + name + "/device/";
            if (Number(device + "vendor", 16) == graphics.vendorId && Number(device + "device", 16) == graphics.deviceId)
            {
                m_device = device;
            }
        }
        if (cards != nullptr)
        {
            closedir(cards);
        }
        DIR * monitors = m_device.empty() ? nullptr : opendir((m_device + "hwmon").c_str());
        for (dirent * entry = monitors != nullptr ? readdir(monitors) : nullptr; entry != nullptr; entry = readdir(monitors))
        {
            if (std::strncmp(entry->d_name, "hwmon", 5) == 0)
            {
                m_monitor = m_device + "hwmon/" + entry->d_name + "/";
                break;
            }
        }
        if (monitors != nullptr)
        {
            closedir(monitors);
        }
    }

    void Sample(MachineUsage & usage)
    {
        uint64_t busy = 0, total = 0;
        if (Times(busy, total) && total > m_total)
        {
            usage.cpu = 100.0 * static_cast<double>(busy - m_busy) / static_cast<double>(total - m_total);
            m_busy = busy;
            m_total = total;
        }
        std::ifstream file("/proc/meminfo");
        uint64_t available = 0;
        for (std::string line; std::getline(file, line);)
        {
            unsigned long long value = 0;
            if (std::sscanf(line.c_str(), "MemTotal: %llu kB", &value) == 1)
            {
                usage.memoryTotal = value * 1024;
            }
            else if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &value) == 1)
            {
                available = value * 1024;
            }
        }
        usage.memoryUsed = usage.memoryTotal > available ? usage.memoryTotal - available : 0;
        if (m_device.empty())
        {
            return;
        }
        const int64_t gpu = Number(m_device + "gpu_busy_percent", 10);
        usage.gpu = gpu >= 0 ? static_cast<double>(gpu) : usage.gpu;
        const int64_t used = Number(m_device + "mem_info_vram_used", 10);
        usage.videoMemoryUsed = used > 0 ? static_cast<uint64_t>(used) : usage.videoMemoryUsed;
        const int64_t vram = Number(m_device + "mem_info_vram_total", 10);
        usage.videoMemoryTotal = vram > 0 ? static_cast<uint64_t>(vram) : usage.videoMemoryTotal;
        if (m_monitor.empty())
        {
            return;
        }
        const int64_t temperature = Number(m_monitor + "temp1_input", 10);
        usage.gpuTemperature = temperature >= 0 ? temperature / 1000.0 : usage.gpuTemperature;
        int64_t power = Number(m_monitor + "power1_average", 10);
        power = power >= 0 ? power : Number(m_monitor + "power1_input", 10);
        usage.gpuPower = power >= 0 ? power / 1e6 : usage.gpuPower;
        const int64_t clock = Number(m_monitor + "freq1_input", 10);
        usage.gpuClock = clock >= 0 ? clock / 1e6 : usage.gpuClock;
    }

private:
    static int64_t Number(const std::string & path, int base)
    {
        std::ifstream file(path);
        std::string text;
        if (!(file >> text))
        {
            return -1;
        }
        char * end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, base);
        return end != text.c_str() ? value : -1;
    }

    static bool Times(uint64_t & busy, uint64_t & total)
    {
        std::ifstream file("/proc/stat");
        std::string cpu;
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
        if (!(file >> cpu >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal) || cpu != "cpu")
        {
            return false;
        }
        busy = user + nice + system + irq + softirq + steal;
        total = busy + idle + wait;
        return true;
    }

    std::string m_device;
    std::string m_monitor;
    uint64_t m_busy = 0, m_total = 0;
};
#endif
}

MachineMonitor::~MachineMonitor()
{
    Stop();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void MachineMonitor::Start(const GraphicsIdentity & graphics)
{
    {
        std::scoped_lock lock(m_lock);
        m_stop = false;
        if (m_running)
        {
            return;
        }
        m_running = true;
        m_usage = MachineUsage();
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    m_thread = std::thread(&MachineMonitor::Run, this, graphics);
}

void MachineMonitor::Stop()
{
    {
        std::scoped_lock lock(m_lock);
        m_stop = true;
    }
    m_wake.notify_all();
}

bool MachineMonitor::Running() const
{
    std::scoped_lock lock(m_lock);
    return m_running && !m_stop;
}

MachineUsage MachineMonitor::Usage() const
{
    std::scoped_lock lock(m_lock);
    return m_usage;
}

void MachineMonitor::Run(GraphicsIdentity graphics)
{
    std::unique_lock lock(m_lock);
    while (!m_stop)
    {
        lock.unlock();
        Sample(graphics);
        lock.lock();
    }
    m_running = false;
}

void MachineMonitor::Sample(const GraphicsIdentity & graphics)
{
    const std::string cpuName = ProcessorName();
    Nvml nvml(graphics);
#if defined(_WIN32)
    Adl adl(graphics);
    MachineUsage vendor;
    nvml.Sample(vendor);
    adl.Sample(vendor);
    SystemCounters system(graphics, vendor.gpu < 0.0);
#else
    SystemCounters system(graphics);
#endif
    std::unique_lock lock(m_lock);
    while (!m_stop)
    {
        lock.unlock();
        MachineUsage usage;
        usage.cpuName = cpuName;
        usage.gpuName = graphics.name;
        usage.videoMemoryTotal = graphics.memory;
        system.Sample(usage);
        nvml.Sample(usage);
#if defined(_WIN32)
        adl.Sample(usage);
#endif
        lock.lock();
        m_usage = usage;
        m_wake.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_stop; });
    }
}
