#include "performance_overlay.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <common/std_string.h>
#include <numeric>
#include <nxemu/ui/host/process_usage.h>
#include <nxemu/settings/ui_settings.h>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr float kFontSize = 13.0f;
constexpr float kGraphHeight = 1.23f;
constexpr int kGraphSteps = 16;
constexpr float kUsualSpeeds[] = {30.0f, 60.0f, 120.0f, 240.0f};
constexpr float kOverSpeed = 1.05f;
constexpr double kSlowSpeed = 0.9;
constexpr const char * kUnknown = "-";
constexpr double kStutterRatio = 2.5;
constexpr double kSlowestGame = 10.0;
constexpr double kFreeze = 0.5;
constexpr float kUsualTimes[] = {1000.0f / 60.0f, 1000.0f / 30.0f, 50.0f, 100.0f};
constexpr int kFrameSteps = 24;


Rml::Element * Part(Rml::Element * root, const char * className)
{
    Rml::ElementList found;
    root->GetElementsByClassName(found, className);
    return found.empty() ? nullptr : found.front();
}

void SetText(Rml::Element * root, const char * className, const std::string & text)
{
    Rml::Element * element = Part(root, className);
    const Rml::String rml = Rml::StringUtilities::EncodeRml(text);
    if (element != nullptr && element->GetInnerRML() != rml)
    {
        element->SetInnerRML(rml);
    }
}

std::string Number(const char * format, double value)
{
    return stdstr_f(format, std::isfinite(value) ? value : 0.0);
}

std::string Value(const char * className, const std::string & unit)
{
    return std::string("<div class=\"perf-value ") + className + "\">" + kUnknown + "</div><div class=\"perf-unit\">" + Rml::StringUtilities::EncodeRml(unit) + "</div>";
}

std::string Plain(const char * className)
{
    return std::string("<div class=\"perf-value ") + className + "\">" + kUnknown + "</div>";
}

std::string FpsCell(const char * className, const char * label)
{
    return std::string("<div class=\"perf-fps-cell\"><div class=\"perf-fps-label\">") + label + "</div>" + Plain(className) + "</div>";
}

struct Rows
{
    bool compact;
    std::string labels;
    std::string values;

    void Add(const std::string & label, const char * group, const std::string & content, bool tall = false)
    {
        if (compact)
        {
            values += content;
            return;
        }
        const std::string size = tall ? " tall" : "";
        labels += std::string("<div class=\"perf-label ") + group + size + "\">" + Rml::StringUtilities::EncodeRml(label) + "</div>";
        values += "<div class=\"perf-values" + size + "\">" + content + "</div>";
    }
};

std::string Bytes(uint64_t bytes)
{
    if (bytes == 0) return kUnknown;
    constexpr double kMegabyte = 1024.0 * 1024.0;
    constexpr double kGigabyte = kMegabyte * 1024.0;
    return bytes >= kGigabyte ? Tr("%1 GB", {Number("%.1f", bytes / kGigabyte)}) : Tr("%1 MB", {Number("%.0f", bytes / kMegabyte)});
}

std::string Share(uint64_t used, uint64_t total)
{
    if (used == 0)
    {
        return kUnknown;
    }
    return total == 0 ? Bytes(used) : Bytes(used) + " / " + Bytes(total);
}

std::string Join(std::initializer_list<std::string> parts)
{
    std::string text;
    for (const std::string & part : parts)
    {
        if (!part.empty())
        {
            text += (text.empty() ? "" : "   ") + part;
        }
    }
    return text.empty() ? kUnknown : text;
}

std::string Measure(const char * format, double value)
{
    return value < 0.0 ? std::string() : Number(format, value);
}

bool & Setting(PerformanceItem item)
{
    switch (item)
    {
    case PerformanceItem::History: return uiSettings.performanceHistory;
    case PerformanceItem::Range: return uiSettings.performanceRange;
    case PerformanceItem::FrameTime: return uiSettings.performanceFrameTime;
    case PerformanceItem::Longest: return uiSettings.performanceLongest;
    case PerformanceItem::Speed: return uiSettings.performanceSpeed;
    case PerformanceItem::Processor: return uiSettings.performanceProcessor;
    case PerformanceItem::FrameGraph: return uiSettings.performanceFrameGraph;
    case PerformanceItem::Lows: return uiSettings.performanceLows;
    case PerformanceItem::GpuTime: return uiSettings.performanceGpuTime;
    case PerformanceItem::Graphics: return uiSettings.performanceGraphics;
    case PerformanceItem::System: return uiSettings.performanceSystem;
    case PerformanceItem::Hardware: return uiSettings.performanceHardware;
    case PerformanceItem::Memory: break;
    }
    return uiSettings.performanceMemory;
}
}

PerformanceOverlay::PerformanceOverlay(Rml::Element * root) :
    m_root(root)
{
}

bool PerformanceOverlay::Compact()
{
    return uiSettings.performanceStyle != 1;
}

bool PerformanceOverlay::Has(PerformanceItem item)
{
    return (!Compact() || item <= PerformanceItem::Lows) && Setting(item);
}

bool PerformanceOverlay::WantsMachine()
{
    return Has(PerformanceItem::Graphics) || Has(PerformanceItem::System) || Has(PerformanceItem::Hardware);
}

int PerformanceOverlay::Size()
{
    return std::clamp(static_cast<int>(uiSettings.performanceSize), kSmallest, kLargest);
}

int PerformanceOverlay::Rate()
{
    return std::clamp(static_cast<int>(uiSettings.performanceRate), kSlowest, kFastest);
}

int PerformanceOverlay::Length()
{
    return std::clamp(static_cast<int>(uiSettings.performanceHistoryLength), kShortest, kLongest);
}

uint32_t PerformanceOverlay::Items()
{
    uint32_t items = 0;
    for (int item = 0; item <= static_cast<int>(PerformanceItem::Hardware); item++)
    {
        items |= Has(static_cast<PerformanceItem>(item)) ? 1u << item : 0u;
    }
    return items;
}

void PerformanceOverlay::Refresh()
{
    if (Size() != m_size)
    {
        m_size = Size();
        m_root->SetProperty(Rml::PropertyId::FontSize, Rml::Property(kFontSize * static_cast<float>(m_size) / kUsualSize, Rml::Unit::DP));
    }
    if (Compact() == m_compact && Items() == m_items && static_cast<size_t>(Length()) == m_length)
    {
        return;
    }
    const size_t kept = std::min(m_count, static_cast<size_t>(Length()));
    std::move(m_fps.begin() + (m_count - kept), m_fps.begin() + m_count, m_fps.begin());
    std::move(m_hitches.begin() + (m_count - kept), m_hitches.begin() + m_count, m_hitches.begin());
    m_count = kept;
    Build();
}

void PerformanceOverlay::Show(bool shown)
{
    if (shown == m_shown)
    {
        return;
    }
    m_shown = shown;
    m_count = 0;
    m_cpuAt = 0;
    m_cpuShare = -1.0;
    m_memory = 0;
    m_frameCount = 0;
    m_framePos = 0;
    m_gpuTime = -1.0;
    m_machine = MachineUsage();
    if (shown)
    {
        m_size = Size();
        m_root->SetProperty(Rml::PropertyId::FontSize, Rml::Property(kFontSize * static_cast<float>(m_size) / kUsualSize, Rml::Unit::DP));
        Build();
    }
    else
    {
        m_root->SetInnerRML("");
    }
    m_root->SetClass("shown", shown);
}

void PerformanceOverlay::Build()
{
    m_length = static_cast<size_t>(Length());
    m_items = Items();
    m_compact = Compact();
    m_root->SetClass("compact", m_compact);
    std::string rml = (m_compact ? "<div class=\"perf-unit\">FPS</div>" + Plain("perf-fps") : Value("perf-fps", "FPS")) + "<div class=\"perf-unlocked\">&#8734;</div>";
    std::string fpsCells;
    if (!m_compact) rml = "<div class=\"perf-head\">" + rml + "<div class=\"perf-shaders\" /></div>";
    Rows rows{m_compact, {}, {}};
    if (Has(PerformanceItem::History))
    {
        std::string graph = "<div class=\"perf-graph\">";
        for (size_t i = 0; i < m_length; i++)
        {
            graph += "<div />";
        }
        rows.Add(Tr("History"), "game", graph + "</div>");
    }
    if (Has(PerformanceItem::Range))
    {
        std::string range;
        for (const char * part : {"low", "mean", "high"})
        {
            if (m_compact)
            {
                const char * label = std::string_view(part) == "low" ? "min" : std::string_view(part) == "mean" ? "avg" : "max";
                fpsCells += FpsCell((std::string("perf-") + part).c_str(), label);
            }
            else
            {
                range += std::string("<div class=\"perf-mark ") + part + "\" /><div class=\"perf-" + part + "\">" + kUnknown + "</div>";
            }
        }
        if (!m_compact) rows.Add(Tr("Lowest, average and highest"), "game", range);
    }
    if (Has(PerformanceItem::FrameTime)) rows.Add(Tr("Frame time"), "game", Value("perf-time", "ms"));
    if (Has(PerformanceItem::Longest)) rows.Add(Tr("Longest frame"), "game", m_compact ? Plain("perf-longest") + "<div class=\"perf-unit perf-stacked-unit\">ms<br />max</div>" : Value("perf-longest", "ms"));
    if (Has(PerformanceItem::Speed)) rows.Add(Tr("Emulation speed"), "game", Value("perf-speed", "%"));
    if (Has(PerformanceItem::FrameGraph))
    {
        std::string graph = "<div class=\"perf-frames\">";
        for (size_t i = 0; i < kFrameBars; i++)
        {
            graph += "<div />";
        }
        rows.Add(Tr("Frame time history"), "game", graph + "</div>", true);
    }
    if (Has(PerformanceItem::Lows))
    {
        if (m_compact) fpsCells += FpsCell("perf-low-1", "1% low");
        else rows.Add(Tr("1% low"), "game", Value("perf-low-1", "FPS"));
        if (!m_compact) rows.Add(Tr("0.1% low"), "game", Value("perf-low-01", "FPS"));
    }
    if (Has(PerformanceItem::GpuTime)) rows.Add(Tr("GPU time"), "time", Value("perf-gpu-time", "ms"));
    if (Has(PerformanceItem::Processor)) rows.Add(Tr("Emulator CPU"), "process", Value("perf-cpu", m_compact ? "% CPU" : "%"));
    if (Has(PerformanceItem::Memory)) rows.Add(Tr("Emulator RAM"), "process", m_compact ? Value("perf-memory", "RAM") : Plain("perf-memory"));
    if (Has(PerformanceItem::Graphics))
    {
        rows.Add("GPU", "gpu", Plain("perf-gpu"));
        rows.Add("VRAM", "gpu", Plain("perf-vram"));
    }
    if (Has(PerformanceItem::System))
    {
        rows.Add(Tr("System CPU"), "system", Plain("perf-system-cpu"));
        rows.Add(Tr("System RAM"), "system", Plain("perf-system-ram"));
    }
    if (m_compact)
    {
        rml = "<div class=\"perf-fps-group\">" + rml + fpsCells + "</div>" + rows.values + "<div class=\"perf-shaders\" />";
    }
    else if (!rows.labels.empty())
    {
        rml += "<div class=\"perf-table\"><div class=\"perf-labels\">" + rows.labels + "</div><div class=\"perf-cells\">" + rows.values + "</div></div>";
    }
    if (Has(PerformanceItem::Hardware)) rml += std::string("<div class=\"perf-name perf-gpu-name\">") + kUnknown + "</div><div class=\"perf-name perf-cpu-name\">" + kUnknown + "</div>";
    m_root->SetInnerRML(rml);
    m_heights.fill(0);
    m_frameHeights.fill(0);
    m_frameHitches.fill(0);
    if (m_count > 0)
    {
        Render();
    }
}

void PerformanceOverlay::Add(const PerfStatsResults & stats, uint32_t shaders, bool unlocked, bool uncapped, const Details & details)
{
    if (!m_shown)
    {
        return;
    }
    Refresh();
    if (m_count == m_length)
    {
        std::move(m_fps.begin() + 1, m_fps.begin() + m_count, m_fps.begin());
        std::move(m_hitches.begin() + 1, m_hitches.begin() + m_count, m_hitches.begin());
        m_count--;
    }
    const double fps = std::isfinite(stats.average_game_fps) ? stats.average_game_fps : 0.0;
    m_fps[m_count] = static_cast<float>(fps);
    m_hitches[m_count++] = stats.longest_game_frame >= kFreeze ? Hitch::Freeze :
                           stats.longest_game_frame * std::max(kSlowestGame, fps) > kStutterRatio ? Hitch::Stutter : Hitch::None;
    m_stats = stats;
    m_shaders = shaders;
    m_unlocked = unlocked;
    m_uncapped = uncapped;
    if (Has(PerformanceItem::Processor))
    {
        const double seconds = ProcessCpuSeconds();
        const uint64_t now = SDL_GetTicks();
        m_cpuShare = m_cpuAt != 0 && now > m_cpuAt ? (seconds - m_cpuSeconds) * 1000.0 / static_cast<double>(now - m_cpuAt) / std::max(1, SDL_GetNumLogicalCPUCores()) : -1.0;
        m_cpuSeconds = seconds;
        m_cpuAt = now;
    }
    else
    {
        m_cpuAt = 0;
        m_cpuShare = -1.0;
    }
    m_memory = Has(PerformanceItem::Memory) ? ProcessMemory() : 0;
    for (uint32_t i = 0; i < details.frameCount; i++)
    {
        m_frames[m_framePos] = details.frames[i];
        m_framePos = (m_framePos + 1) % kFramesKept;
        m_frameCount = std::min(m_frameCount + 1, kFramesKept);
    }
    m_gpuTime = details.gpuTime;
    m_machine = details.machine;
    Render();
}

void PerformanceOverlay::Render()
{
    const auto first = m_fps.begin();
    const auto last = first + m_count;
    const auto [lowest, highest] = std::minmax_element(first, last);
    SetText(m_root, "perf-fps", Number("%.0f", *(last - 1)));
    SetText(m_root, "perf-low", Number("%.0f", *lowest));
    SetText(m_root, "perf-mean", Number("%.0f", std::accumulate(first, last, 0.0) / static_cast<double>(m_count)));
    SetText(m_root, "perf-high", Number("%.0f", *highest));
    SetText(m_root, "perf-time", Number("%.1f", m_stats.frametime * 1000.0));
    SetText(m_root, "perf-longest", Number("%.0f", m_stats.longest_game_frame * 1000.0));
    SetText(m_root, "perf-speed", Number("%.0f", m_stats.emulation_speed * 100.0));
    SetText(m_root, "perf-cpu", m_cpuShare < 0.0 ? kUnknown : Number("%.0f", m_cpuShare * 100.0));
    SetText(m_root, "perf-memory", m_compact ? Bytes(m_memory) : Share(m_memory, static_cast<uint64_t>(std::max(0, SDL_GetSystemRAM())) * 1024 * 1024));
    m_root->SetClass("stutter", m_hitches[m_count - 1] == Hitch::Stutter);
    m_root->SetClass("freeze", m_hitches[m_count - 1] == Hitch::Freeze);
    m_root->SetClass("unlocked", m_unlocked);
    m_root->SetClass("uncapped", m_unlocked && m_uncapped);
    m_root->SetClass("slow", std::isfinite(m_stats.emulation_speed) && m_stats.emulation_speed < kSlowSpeed);
    SetText(m_root, "perf-shaders", m_shaders == 0 ? std::string() : TrCount("Building: %n shader", "Building: %n shaders", m_shaders));

    RenderDetails();
    Rml::Element * graph = Part(m_root, "perf-graph");
    if (graph == nullptr)
    {
        return;
    }
    float top = std::max(1.0f, *highest);
    for (auto speed = std::rbegin(kUsualSpeeds); speed != std::rend(kUsualSpeeds) && *highest <= *speed * kOverSpeed; ++speed)
    {
        top = *speed;
    }
    const size_t offset = m_length - m_count;
    for (size_t i = offset; i < m_length; i++)
    {
        Rml::Element * bar = graph->GetChild(static_cast<int>(i));
        const int height = std::clamp(static_cast<int>(std::lround(kGraphSteps * m_fps[i - offset] / top)), 1, kGraphSteps);
        if (height != m_heights[i])
        {
            m_heights[i] = height;
            bar->SetProperty(Rml::PropertyId::Height, Rml::Property(kGraphHeight * static_cast<float>(height) / kGraphSteps, Rml::Unit::EM));
        }
        bar->SetClass("stutter", m_hitches[i - offset] == Hitch::Stutter);
        bar->SetClass("freeze", m_hitches[i - offset] == Hitch::Freeze);
    }
}

void PerformanceOverlay::RenderDetails()
{
    const auto frame = [this](size_t age) { return m_frames[(m_framePos + kFramesKept - 1 - age) % kFramesKept]; };
    if (Has(PerformanceItem::Lows))
    {
        std::vector<float> slowest(m_frameCount);
        for (size_t i = 0; i < m_frameCount; i++)
        {
            slowest[i] = frame(i);
        }
        std::sort(slowest.begin(), slowest.end(), std::greater<float>());
        const auto low = [&slowest](size_t share) {
            const size_t count = std::max<size_t>(1, slowest.size() / share);
            const double mean = std::accumulate(slowest.begin(), slowest.begin() + count, 0.0) / static_cast<double>(count);
            return mean > 0.0 ? Number("%.0f", 1000.0 / mean) : std::string(kUnknown);
        };
        SetText(m_root, "perf-low-1", slowest.empty() ? kUnknown : low(100));
        SetText(m_root, "perf-low-01", slowest.empty() ? kUnknown : low(1000));
    }
    if (Has(PerformanceItem::GpuTime))
    {
        SetText(m_root, "perf-gpu-time", m_gpuTime < 0.0 ? kUnknown : Number("%.2f", m_gpuTime));
    }
    if (Has(PerformanceItem::Graphics))
    {
        SetText(m_root, "perf-gpu", Join({Measure("%.0f%%", m_machine.gpu), Measure("%.0f \xC2\xB0" "C", m_machine.gpuTemperature), Measure("%.0f W", m_machine.gpuPower), Measure("%.0f MHz", m_machine.gpuClock)}));
        SetText(m_root, "perf-vram", Share(m_machine.videoMemoryUsed, m_machine.videoMemoryTotal));
    }
    if (Has(PerformanceItem::System))
    {
        SetText(m_root, "perf-system-cpu", m_machine.cpu < 0.0 ? kUnknown : Number("%.0f%%", m_machine.cpu));
        SetText(m_root, "perf-system-ram", Share(m_machine.memoryUsed, m_machine.memoryTotal));
    }
    if (Has(PerformanceItem::Hardware))
    {
        SetText(m_root, "perf-gpu-name", m_machine.gpuName.empty() ? kUnknown : m_machine.gpuName);
        SetText(m_root, "perf-cpu-name", m_machine.cpuName.empty() ? kUnknown : m_machine.cpuName);
    }

    Rml::Element * graph = Part(m_root, "perf-frames");
    if (graph == nullptr)
    {
        return;
    }
    const size_t shown = std::min(m_frameCount, kFrameBars);
    float highest = 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < shown; i++)
    {
        highest = std::max(highest, frame(i));
        sum += frame(i);
    }
    const double mean = shown != 0 ? sum / static_cast<double>(shown) : 0.0;
    float top = std::max(1.0f, highest);
    for (auto time = std::rbegin(kUsualTimes); time != std::rend(kUsualTimes) && highest <= *time * kOverSpeed; ++time)
    {
        top = *time;
    }
    for (size_t i = 0; i < kFrameBars; i++)
    {
        Rml::Element * bar = graph->GetChild(static_cast<int>(kFrameBars - 1 - i));
        const float time = i < shown ? frame(i) : 0.0f;
        const int height = i < shown ? std::clamp(static_cast<int>(std::lround(kFrameSteps * time / top)), 1, kFrameSteps) : 0;
        if (height != m_frameHeights[i])
        {
            m_frameHeights[i] = height;
            bar->SetProperty(Rml::PropertyId::Height, Rml::Property(kGraphHeight * (m_compact ? 1.0f : 2.0f) * static_cast<float>(height) / kFrameSteps, Rml::Unit::EM));
        }
        const uint8_t hitch = static_cast<uint8_t>(i >= shown ? Hitch::None : time >= kFreeze * 1000.0 ? Hitch::Freeze : time > mean * kStutterRatio ? Hitch::Stutter : Hitch::None);
        if (hitch != m_frameHitches[i])
        {
            m_frameHitches[i] = hitch;
            bar->SetClass("stutter", hitch == static_cast<uint8_t>(Hitch::Stutter));
            bar->SetClass("freeze", hitch == static_cast<uint8_t>(Hitch::Freeze));
        }
    }
}
