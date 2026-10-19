#include "performance_overlay.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <common/std_string.h>
#include <numeric>
#include <nxemu/ui/host/process_usage.h>
#include <nxemu/settings/ui_settings.h>
#include <string>

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
    case PerformanceItem::Memory: break;
    }
    return uiSettings.performanceMemory;
}
}

PerformanceOverlay::PerformanceOverlay(Rml::Element * root) :
    m_root(root)
{
}

bool PerformanceOverlay::Has(PerformanceItem item)
{
    return Setting(item);
}

void PerformanceOverlay::Toggle(PerformanceItem item)
{
    Setting(item) = !Setting(item);
    SaveUISetting();
    if (m_shown)
    {
        Build();
    }
}

int PerformanceOverlay::Size()
{
    return std::clamp(static_cast<int>(uiSettings.performanceSize), kSmallest, kLargest);
}

int PerformanceOverlay::Rate()
{
    return std::clamp(static_cast<int>(uiSettings.performanceRate), kSlowest, kFastest);
}

void PerformanceOverlay::SetRate(int rate)
{
    if (rate != Rate())
    {
        uiSettings.performanceRate = rate;
        SaveUISetting();
        m_interval = 1000 / static_cast<uint64_t>(Rate());
    }
}

int PerformanceOverlay::Length()
{
    return std::clamp(static_cast<int>(uiSettings.performanceHistoryLength), kShortest, kLongest);
}

void PerformanceOverlay::SetSize(int size)
{
    if (size != Size())
    {
        uiSettings.performanceSize = size;
        SaveUISetting();
        m_root->SetProperty(Rml::PropertyId::FontSize, Rml::Property(kFontSize * static_cast<float>(Size()) / kUsualSize, Rml::Unit::DP));
    }
}

void PerformanceOverlay::SetLength(int length)
{
    if (length == Length())
    {
        return;
    }
    uiSettings.performanceHistoryLength = length;
    SaveUISetting();
    if (m_shown)
    {
        const size_t kept = std::min(m_count, static_cast<size_t>(Length()));
        std::move(m_fps.begin() + (m_count - kept), m_fps.begin() + m_count, m_fps.begin());
        std::move(m_hitches.begin() + (m_count - kept), m_hitches.begin() + m_count, m_hitches.begin());
        m_count = kept;
        Build();
    }
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
    if (shown)
    {
        m_root->SetProperty(Rml::PropertyId::FontSize, Rml::Property(kFontSize * static_cast<float>(Size()) / kUsualSize, Rml::Unit::DP));
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
    m_interval = 1000 / static_cast<uint64_t>(Rate());
    std::string rml = Value("perf-fps", "FPS") + "<div class=\"perf-unlocked\">&#8734;</div>";
    if (Has(PerformanceItem::History))
    {
        rml += "<div class=\"perf-graph\">";
        for (size_t i = 0; i < m_length; i++)
        {
            rml += "<div />";
        }
        rml += "</div>";
    }
    if (Has(PerformanceItem::Range))
    {
        for (const char * part : {"low", "mean", "high"})
        {
            rml += std::string("<div class=\"perf-mark ") + part + "\" /><div class=\"perf-" + part + "\">" + kUnknown + "</div>";
        }
    }
    if (Has(PerformanceItem::FrameTime)) rml += Value("perf-time", "ms");
    if (Has(PerformanceItem::Longest)) rml += Value("perf-longest", "ms max");
    if (Has(PerformanceItem::Speed)) rml += Value("perf-speed", "%");
    if (Has(PerformanceItem::Processor)) rml += Value("perf-cpu", "% CPU");
    if (Has(PerformanceItem::Memory)) rml += Value("perf-memory", "RAM");
    rml += "<div class=\"perf-shaders\" />";
    m_root->SetInnerRML(rml);
    m_heights.fill(0);
    if (m_count > 0)
    {
        Render();
    }
}

void PerformanceOverlay::Add(const PerfStatsResults & stats, uint32_t shaders, bool unlocked)
{
    if (!m_shown)
    {
        return;
    }
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
    if (Has(PerformanceItem::Processor))
    {
        const double seconds = ProcessCpuSeconds();
        const uint64_t now = SDL_GetTicks64();
        m_cpuShare = m_cpuAt != 0 && now > m_cpuAt ? (seconds - m_cpuSeconds) * 1000.0 / static_cast<double>(now - m_cpuAt) / std::max(1, SDL_GetCPUCount()) : -1.0;
        m_cpuSeconds = seconds;
        m_cpuAt = now;
    }
    else
    {
        m_cpuAt = 0;
        m_cpuShare = -1.0;
    }
    m_memory = Has(PerformanceItem::Memory) ? ProcessMemory() : 0;
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
    constexpr double kMegabyte = 1024.0 * 1024.0;
    constexpr double kGigabyte = kMegabyte * 1024.0;
    SetText(m_root, "perf-memory", m_memory == 0 ? kUnknown : m_memory >= kGigabyte ? Tr("%1 GB", {Number("%.1f", m_memory / kGigabyte)}) : Tr("%1 MB", {Number("%.0f", m_memory / kMegabyte)}));
    m_root->SetClass("stutter", m_hitches[m_count - 1] == Hitch::Stutter);
    m_root->SetClass("freeze", m_hitches[m_count - 1] == Hitch::Freeze);
    m_root->SetClass("unlocked", m_unlocked);
    m_root->SetClass("slow", std::isfinite(m_stats.emulation_speed) && m_stats.emulation_speed < kSlowSpeed);
    SetText(m_root, "perf-shaders", m_shaders == 0 ? std::string() : TrCount("Building: %n shader", "Building: %n shaders", m_shaders));

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
