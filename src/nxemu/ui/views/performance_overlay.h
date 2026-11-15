#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu/ui/host/machine_usage.h>
#include <nxemu/ui/i18n/translation.h>

namespace Rml
{
class Element;
}

enum class PerformanceItem
{
    History,
    Range,
    FrameTime,
    Longest,
    Speed,
    Processor,
    Memory,
    FrameGraph,
    Lows,
    GpuTime,
    Graphics,
    System,
    Hardware,
};

class PerformanceOverlay final
{
public:
    struct Details
    {
        const float * frames = nullptr;
        uint32_t frameCount = 0;
        double gpuTime = -1.0;
        MachineUsage machine;
    };

    explicit PerformanceOverlay(Rml::Element * root);

    void Show(bool shown);
    bool Shown() const { return m_shown; }

    static bool Has(PerformanceItem item);
    static bool WantsMachine();

    static constexpr int kSmallest = 75, kUsualSize = 100, kLargest = 200;
    static constexpr int kSlowest = 1, kUsualRate = 2, kFastest = 10;
    static constexpr int kShortest = 10, kUsualLength = 30, kLongest = 60;
    static int Size();
    static int Rate();
    static int Length();
    uint64_t Interval() const { return 1000 / static_cast<uint64_t>(Rate()); }

    void Add(const PerfStatsResults & stats, uint32_t shaders, bool unlocked, bool uncapped, const Details & details);

private:
    PerformanceOverlay(const PerformanceOverlay &) = delete;
    PerformanceOverlay & operator=(const PerformanceOverlay &) = delete;

    static bool Compact();
    static uint32_t Items();
    void Refresh();
    void Build();
    void Render();
    void RenderDetails();

    enum class Hitch : uint8_t { None, Stutter, Freeze };

    Rml::Element * const m_root;
    bool m_shown = false;
    bool m_compact = true;
    std::array<float, kLongest> m_fps{};
    std::array<Hitch, kLongest> m_hitches{};
    std::array<int, kLongest> m_heights{};
    size_t m_length = kUsualLength;
    uint32_t m_items = 0;
    int m_size = kUsualSize;
    size_t m_count = 0;
    PerfStatsResults m_stats{};
    uint32_t m_shaders = 0;
    bool m_unlocked = false;
    bool m_uncapped = false;
    double m_cpuSeconds = 0.0;
    uint64_t m_cpuAt = 0;
    double m_cpuShare = -1.0;
    uint64_t m_memory = 0;
    static constexpr size_t kFrameBars = 100, kFramesKept = 1000;
    std::array<float, kFramesKept> m_frames{};
    size_t m_frameCount = 0;
    size_t m_framePos = 0;
    std::array<int, kFrameBars> m_frameHeights{};
    std::array<uint8_t, kFrameBars> m_frameHitches{};
    double m_gpuTime = -1.0;
    MachineUsage m_machine;
};
