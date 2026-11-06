#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <nxemu-module-spec/operating_system.h>
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
};

class PerformanceOverlay final
{
public:
    struct Item
    {
        PerformanceItem id;
        const char * label;
    };
    static constexpr Item kItems[] = {
        {PerformanceItem::History, TrMark("History")},
        {PerformanceItem::Range, TrMark("Lowest, average and highest")},
        {PerformanceItem::FrameTime, TrMark("Frame time")},
        {PerformanceItem::Longest, TrMark("Longest frame")},
        {PerformanceItem::Speed, TrMark("Emulation speed")},
        {PerformanceItem::Processor, TrMark("Processor use")},
        {PerformanceItem::Memory, TrMark("Memory use")},
    };

    explicit PerformanceOverlay(Rml::Element * root);

    void Show(bool shown);
    bool Shown() const { return m_shown; }

    static bool Has(PerformanceItem item);
    void Toggle(PerformanceItem item);

    static constexpr int kSmallest = 75, kUsualSize = 100, kLargest = 200, kSizeStep = 5;
    static constexpr int kSlowest = 1, kUsualRate = 2, kFastest = 10;
    static constexpr int kShortest = 10, kUsualLength = 30, kLongest = 60;
    static int Size();
    static int Rate();
    static int Length();
    void SetSize(int size);
    void SetRate(int rate);
    void SetLength(int length);
    uint64_t Interval() const { return m_interval; }

    void Add(const PerfStatsResults & stats, uint32_t shaders, bool unlocked, bool uncapped);

private:
    PerformanceOverlay(const PerformanceOverlay &) = delete;
    PerformanceOverlay & operator=(const PerformanceOverlay &) = delete;

    void Build();
    void Render();

    enum class Hitch : uint8_t { None, Stutter, Freeze };

    Rml::Element * const m_root;
    bool m_shown = false;
    std::array<float, kLongest> m_fps{};
    std::array<Hitch, kLongest> m_hitches{};
    std::array<int, kLongest> m_heights{};
    size_t m_length = kUsualLength;
    uint64_t m_interval = 1000 / kUsualRate;
    size_t m_count = 0;
    PerfStatsResults m_stats{};
    uint32_t m_shaders = 0;
    bool m_unlocked = false;
    bool m_uncapped = false;
    double m_cpuSeconds = 0.0;
    uint64_t m_cpuAt = 0;
    double m_cpuShare = -1.0;
    uint64_t m_memory = 0;
};
