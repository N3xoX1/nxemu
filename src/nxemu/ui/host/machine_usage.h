#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct GraphicsIdentity
{
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint8_t luid[8] = {};
    bool hasLuid = false;
    uint64_t memory = 0;
};

struct MachineUsage
{
    std::string cpuName;
    std::string gpuName;
    double cpu = -1.0;
    uint64_t memoryUsed = 0;
    uint64_t memoryTotal = 0;
    double gpu = -1.0;
    uint64_t videoMemoryUsed = 0;
    uint64_t videoMemoryTotal = 0;
    double gpuTemperature = -1.0;
    double gpuPower = -1.0;
    double gpuClock = -1.0;
};

class MachineMonitor final
{
public:
    MachineMonitor() = default;
    ~MachineMonitor();

    void Start(const GraphicsIdentity & graphics);
    void Stop();
    bool Running() const;
    MachineUsage Usage() const;

private:
    MachineMonitor(const MachineMonitor &) = delete;
    MachineMonitor & operator=(const MachineMonitor &) = delete;

    void Run(GraphicsIdentity graphics);
    void Sample(const GraphicsIdentity & graphics);

    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_stop = false;
    bool m_running = false;
    MachineUsage m_usage;
};
