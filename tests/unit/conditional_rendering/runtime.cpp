// Host-side control-flow tests; method definitions come directly from vk_query_cache.cpp.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include <vulkan/vulkan_core.h>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using DAddr = u64;
namespace Settings {
bool high = true;
bool IsGPULevelHigh() { return high; }
}
namespace VideoCommon {
struct LookupData { DAddr address; void* found_query; };
enum class ObtainBufferSynchronize { FullSynchronize, NoSynchronize };
enum class ObtainBufferOperation { DoNothing };
}
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Commands {
    bool running{};
    unsigned begins{}, ends{}, resolves{};
    VkConditionalRenderingFlagsEXT flags{};
};
namespace vk {
struct CommandBuffer {
    Commands& commands;
    void BeginConditionalRenderingEXT(const VkConditionalRenderingBeginInfoEXT& info) {
        Check(!commands.running, "nested conditional begin");
        commands.running = true;
        ++commands.begins;
        commands.flags = info.flags;
    }
    void EndConditionalRenderingEXT() {
        Check(commands.running, "unmatched conditional end");
        commands.running = false;
        ++commands.ends;
    }
};
}
struct Scheduler {
    Commands commands;
    template<class F> void Record(F f) { f(vk::CommandBuffer{commands}); }
};
struct Device {
    bool supported = true;
    VkDriverId driver = VK_DRIVER_ID_MESA_RADV;
    bool IsExtConditionalRendering() const { return supported; }
    VkDriverId GetDriverID() const { return driver; }
};
struct Memory {
    std::map<DAddr, u64> values;
    template<class T> T* GetPointer(DAddr address) {
        auto it = values.find(address);
        return it == values.end() ? nullptr : reinterpret_cast<T*>(&it->second);
    }
};
struct Buffer {
    VkBuffer Handle() const { return reinterpret_cast<VkBuffer>(std::uintptr_t{1}); }
};
struct BufferCache {
    std::mutex mutex;
    std::set<DAddr> modified;
    Buffer buffer;
    bool IsRegionGpuModified(DAddr address, size_t) { return modified.contains(address); }
    auto ObtainCPUBuffer(DAddr address, size_t, VideoCommon::ObtainBufferSynchronize,
                         VideoCommon::ObtainBufferOperation) {
        return std::pair{&buffer, VkDeviceSize{address}};
    }
};
struct ResolvePass {
    Commands& commands;
    void Resolve(VkBuffer, VkBuffer, u32, bool) {
        Check(!commands.running, "resolve under old condition");
        ++commands.resolves;
    }
};
struct Impl {
    Device device;
    BufferCache buffer_cache;
    Memory device_memory;
    Scheduler scheduler;
    ResolvePass pass{scheduler.commands};
    ResolvePass* conditional_resolve_pass = &pass;
    VkBuffer resolve = reinterpret_cast<VkBuffer>(std::uintptr_t{2});
    VkBuffer* hcr_resolve_buffer = &resolve;
    VkConditionalRenderingBeginInfoEXT hcr_setup{
        VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
    VkBuffer hcr_buffer{};
    size_t hcr_offset{};
    bool hcr_is_set{}, is_hcr_running{};
};
class QueryCacheRuntime {
public:
    Impl* impl;
    void EndHostConditionalRendering();
    void PauseHostConditionalRendering();
    void ResumeHostConditionalRendering();
    void HostConditionalRenderingCompareValueImpl(VideoCommon::LookupData, bool);
    void HostConditionalRenderingCompareBCImpl(DAddr, bool);
    bool HostConditionalRenderingCompareValue(VideoCommon::LookupData, bool);
    bool HostConditionalRenderingCompareValues(VideoCommon::LookupData,
                                              VideoCommon::LookupData, bool, bool);
};
#include "runtime.inc"

unsigned checks{}, failures{};
template<class F> void Test(const char* label, F f) {
    ++checks;
    try { f(); } catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", label, e.what());
    }
}
int main() {
    using VideoCommon::LookupData;
    for (const auto driver : {VK_DRIVER_ID_MESA_RADV, VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
                             VK_DRIVER_ID_ARM_PROPRIETARY, VK_DRIVER_ID_MESA_TURNIP,
                             VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS}) {
        for (const bool high : {false, true}) {
            for (const bool equal : {false, true}) {
                for (const bool paused : {false, true}) {
                    Test("conditional -> bypass -> conditional", [&] {
                        Settings::high = high;
                        Impl impl;
                        impl.device.driver = driver;
                        QueryCacheRuntime runtime{&impl};
                        LookupData a{0x100, &impl}, b{0x110, &impl};
                        runtime.HostConditionalRenderingCompareValue(a, true);
                        if (paused) runtime.PauseHostConditionalRendering();
                        Check(runtime.HostConditionalRenderingCompareValues(a, b, true, equal),
                              "bypass return value");
                        Check(!impl.hcr_is_set && !impl.is_hcr_running,
                              "old condition leaked through bypass");
                        Check(impl.scheduler.commands.ends == 1, "end count after bypass");
                        runtime.HostConditionalRenderingCompareValues(a, b, true, equal);
                        runtime.EndHostConditionalRendering();
                        Check(impl.scheduler.commands.ends == 1, "duplicate end on repeated bypass");
                        runtime.HostConditionalRenderingCompareValue(a, true);
                        Check(impl.scheduler.commands.begins == 2, "condition did not restart");
                        runtime.EndHostConditionalRendering();
                    });
                }
            }
        }
    }
    for (const bool equal : {false, true}) {
        Test("Normal with nonzero CPU operand", [&] {
            Settings::high = false;
            Impl impl;
            QueryCacheRuntime runtime{&impl};
            LookupData a{0x100, nullptr}, b{0x110, nullptr};
            impl.buffer_cache.modified.insert(a.address);
            impl.device_memory.values[b.address] = 42;
            runtime.HostConditionalRenderingCompareValue(a, true);
            runtime.HostConditionalRenderingCompareValues(a, b, true, equal);
            Check(!impl.hcr_is_set && !impl.is_hcr_running, "Normal kept old condition");
        });
        for (const bool high : {false, true}) {
            Test("zero operand replaces condition/polarity", [&] {
                Settings::high = high;
                Impl impl;
                QueryCacheRuntime runtime{&impl};
                LookupData a{0x100, &impl}, b{0x110, nullptr};
                impl.device_memory.values[b.address] = 0;
                runtime.HostConditionalRenderingCompareValue(a, true);
                runtime.HostConditionalRenderingCompareValues(a, b, true, equal);
                const auto expected = equal ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0;
                Check(impl.scheduler.commands.flags == expected, "cached condition ignored polarity");
                Check(impl.is_hcr_running, "replacement not running");
                runtime.HostConditionalRenderingCompareValue(a, true);
                Check(impl.scheduler.commands.flags == 0, "Conditional retained inverted polarity");
                const auto begins = impl.scheduler.commands.begins;
                runtime.PauseHostConditionalRendering();
                runtime.HostConditionalRenderingCompareValue(a, true);
                Check(impl.scheduler.commands.begins == begins + 1, "same predicate did not resume");
                runtime.HostConditionalRenderingCompareValue(a, true);
                Check(impl.scheduler.commands.begins == begins + 1, "same predicate restarted");
                runtime.EndHostConditionalRendering();
            });
        }
        Test("High buffer-cache resolve", [&] {
            Settings::high = true;
            Impl impl;
            QueryCacheRuntime runtime{&impl};
            LookupData a{0x100, nullptr}, b{0x110, nullptr};
            impl.buffer_cache.modified = {a.address, b.address};
            runtime.HostConditionalRenderingCompareValue(a, true);
            runtime.HostConditionalRenderingCompareValues(a, b, true, equal);
            Check(impl.scheduler.commands.resolves == 1, "missing GPU resolve");
            Check(impl.scheduler.commands.ends == 1 && impl.is_hcr_running, "resolve lifecycle");
            runtime.EndHostConditionalRendering();
        });
    }
    for (const bool dirty : {false, true}) {
        Test("CPU fallback closes condition", [&] {
            Impl impl;
            QueryCacheRuntime runtime{&impl};
            LookupData a{0x100, dirty ? nullptr : &impl}, b{0x110, nullptr};
            runtime.HostConditionalRenderingCompareValue(a, true);
            Check(!runtime.HostConditionalRenderingCompareValues(a, b, dirty, false), "fallback return");
            Check(!impl.hcr_is_set && !impl.is_hcr_running, "CPU fallback kept condition");
            runtime.EndHostConditionalRendering();
            Check(impl.scheduler.commands.ends == 1, "fallback duplicate end");
        });
    }
    Test("extension unavailable", [&] {
        Impl impl;
        impl.device.supported = false;
        QueryCacheRuntime runtime{&impl};
        Check(!runtime.HostConditionalRenderingCompareValue({0x100, nullptr}, true), "unsupported single");
        Check(!runtime.HostConditionalRenderingCompareValues({0x100, nullptr}, {0x110, nullptr},
                                                              true, true), "unsupported pair");
        runtime.EndHostConditionalRendering();
        runtime.EndHostConditionalRendering();
        Check(impl.scheduler.commands.begins == 0 && impl.scheduler.commands.ends == 0,
              "extension called on unsupported device");
    });
    std::printf("conditional rendering: %u/%u PASS (production bodies, mocked Vulkan)\n",
                checks - failures, checks);
    return failures != 0;
}
