#pragma once
#include <functional>
#include <common/json.h>
#include <nxemu-module-spec/performance_capture.h>

namespace Core {

struct PerformanceCaptureProcessMemorySample {
    uint64_t working_set_bytes{};
    uint64_t private_bytes{};
};

// Lightweight host-process sample used by the capture recorder. Returns false when unsupported.
bool QueryPerformanceCaptureProcessMemory(PerformanceCaptureProcessMemorySample& sample);
// Lifecycle and file I/O are serialized here; producers only access the shared state.
class PerformanceCapture {
public:
    PerformanceCaptureSharedState& SharedState() { return shared; }
    const PerformanceCaptureSharedState& SharedState() const { return shared; }
    bool IsActive() const { return shared.Epoch() != 0 || pending.load(std::memory_order_acquire); }
    void SetGameVersion(uint32_t version, std::string display) {
        std::scoped_lock lock(lifecycle);
        game_version = version; game_display_version = std::move(display);
    }
    void SetDevice(const char* model, const char* driver) { shared.SetDevice(model, driver); }
    bool Start(uint64_t title, const PerformanceCaptureConfig& config, const std::function<std::chrono::microseconds()>& system_time);
    bool Stop(const std::function<std::chrono::microseconds()>& system_time, std::string& output_path);
private:
    PerformanceCaptureSharedState shared;
    std::mutex lifecycle;
    std::atomic<bool> pending{};
    PerformanceCaptureConfig config{};
    uint64_t title{};
    uint32_t game_version{};
    std::string game_display_version;
    std::chrono::microseconds start_system{}, stop_system{};
    PerformanceCaptureSharedState::Clock::time_point stopped{};
    uint64_t process_cpu_start_100ns{}, process_cpu_stop_100ns{};
    bool process_cpu_available{};
    std::string started_utc;
};

} // namespace Core
