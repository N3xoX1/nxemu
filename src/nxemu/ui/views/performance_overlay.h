#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu/ui/i18n/translation.h>

namespace Rml
{
class Element;
} // namespace Rml

// What the overlay can say beside the pictures a second, in the order it
// says them.
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

// How the game runs, as one line in a corner of it: its pictures a second
// and what the user chose to have with them. It is for when the status bar
// is hidden. Hidden, it holds no element and does nothing.
class PerformanceOverlay final
{
public:
    struct Item
    {
        PerformanceItem id;
        const char * name; // in the settings file
        const char * label;
        bool usual; // shown unless the user chose otherwise
    };
    static constexpr Item kItems[] = {
        {PerformanceItem::History, "History", TrMark("History"), true},
        {PerformanceItem::Range, "Range", TrMark("Lowest, average and highest"), false},
        {PerformanceItem::FrameTime, "FrameTime", TrMark("Frame time"), false},
        {PerformanceItem::Longest, "LongestFrame", TrMark("Longest frame"), true},
        {PerformanceItem::Speed, "Speed", TrMark("Emulation speed"), false},
        {PerformanceItem::Processor, "Processor", TrMark("Processor use"), false},
        {PerformanceItem::Memory, "Memory", TrMark("Memory use"), false},
    };

    // `root` is filled while the overlay is shown. The overlay lives as
    // long as `root`.
    explicit PerformanceOverlay(Rml::Element * root);

    // Showing it starts from nothing: what was measured before is forgotten.
    void Show(bool shown);
    bool Shown() const { return m_shown; }

    // Whether it says the item, which the settings keep.
    static bool Has(PerformanceItem item);
    void Toggle(PerformanceItem item);

    // How large it is, in hundredths of its usual size, how many measures
    // a second it is to be given and how many its history holds: the
    // settings keep them.
    static constexpr int kSmallest = 75, kUsualSize = 100, kLargest = 200, kSizeStep = 5;
    static constexpr int kSlowest = 1, kUsualRate = 2, kFastest = 10;
    static constexpr int kShortest = 10, kUsualLength = 30, kLongest = 60;
    static int Size();
    static int Rate();
    static int Length();
    void SetSize(int size);
    void SetRate(int rate);
    void SetLength(int length);
    // The time from a measure to the next, in milliseconds, while it is shown.
    uint64_t Interval() const { return m_interval; }

    // One more measure, of the time since the last one. `shaders` is how
    // many the game waits for, and `unlocked` whether its speed is not held
    // to that of the console.
    void Add(const PerfStatsResults & stats, uint32_t shaders, bool unlocked);

private:
    PerformanceOverlay(const PerformanceOverlay &) = delete;
    PerformanceOverlay & operator=(const PerformanceOverlay &) = delete;

    void Build();
    void Render();

    // A picture that stayed too long: for a moment, or for so long that the
    // game had stopped.
    enum class Hitch : uint8_t { None, Stutter, Freeze };

    Rml::Element * const m_root;
    bool m_shown = false;
    // The last measures of the pictures a second, the oldest first, with
    // what went wrong then, and the height of the bar each has. Only the
    // first `m_length` places are used.
    std::array<float, kLongest> m_fps{};
    std::array<Hitch, kLongest> m_hitches{};
    std::array<int, kLongest> m_heights{};
    size_t m_length = kUsualLength;
    uint64_t m_interval = 1000 / kUsualRate;
    size_t m_count = 0;
    // The last measure, said again when what is shown changes.
    PerfStatsResults m_stats{};
    uint32_t m_shaders = 0;
    bool m_unlocked = false;
    // The processor time the program had used at the last measure, and when.
    double m_cpuSeconds = 0.0;
    uint64_t m_cpuAt = 0;
    double m_cpuShare = -1.0; // of all the processors; negative while unknown
    uint64_t m_memory = 0;    // bytes the program holds; 0 while unknown
};
