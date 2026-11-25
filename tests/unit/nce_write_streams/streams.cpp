// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic NCE write fault model: a guest fills GPU-cached pages, each write to a protected
// page faults, and the fault handler may invalidate the next pages ahead of the writer.
// Compares no coalescing against the production coalescing (four streams, buffer cache only,
// production NceWriteStreams and BufferCache::WriteMemoryAhead, stopping before aliased pages).
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "yuzu_common/common_types.h"
#include "nxemu-os/core/nce_write_streams.h"

namespace Core {
constexpr u64 DEVICE_PAGESIZE = 0x1000;
}

namespace {

constexpr u64 Page = Core::DEVICE_PAGESIZE;
constexpr u64 MaxWritablePages = Core::NceWriteStreams<>::MaxWritablePages;

unsigned checks{};

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(what);
    }
}

struct PageState {
    bool buffer = false;   // the buffer cache holds a clean copy and protects the page
    bool texture = false;  // another cache (texture, shader) holds and protects the page
    bool gpu_data = false; // GPU-written buffer bytes not in guest memory yet, kept protected
    bool aliased = false;  // a second GPU device mapping also backs this guest page
    bool Protected() const {
        return buffer || texture || gpu_data || aliased;
    }
};

struct Metrics {
    u64 faults = 0;
    u64 ahead_pages = 0;      // pages invalidated before the guest reached them
    u64 lost_gpu_pages = 0;   // GPU-written data dropped ahead of any guest write
    u64 lost_texture_pages = 0; // other caches dropped ahead of any guest write
    u64 lost_alias_pages = 0; // aliased device mappings skipped ahead of any guest write
    u64 wasted_pages = 0;     // invalidated ahead, never written: uploaded again for nothing
};

struct World {
    std::vector<PageState> pages;
    std::vector<bool> written;
    std::vector<bool> ahead;
    Metrics metrics;
};

} // namespace

// The members of the production buffer cache WriteMemoryAhead reads, over the page model
template <class P>
struct BufferCache {
    explicit BufferCache(World* world_) : world{world_} {}

    World* world;

    bool AnyGpuData(DAddr addr, u64 size) const {
        for (u64 page = addr / Page; page < (addr + size + Page - 1) / Page; ++page) {
            if (world->pages[page].gpu_data) {
                return true;
            }
        }
        return false;
    }

    struct Tracker {
        BufferCache* cache;
        bool IsRegionGpuModified(DAddr addr, u64 size) const {
            return cache->AnyGpuData(addr, size);
        }
    } memory_tracker{this};
    struct Ranges {
        bool Empty() const {
            return false;
        }
    } gpu_modified_ranges;
    struct History {
        bool MayHaveWrites(DAddr, u64) const {
            return true;
        }
    } gpu_write_history;

    bool AwaitsProtectedDownload(DAddr, u64) const {
        return false;
    }
    bool IsRegionGpuModified(DAddr addr, u64 size) const {
        return AnyGpuData(addr, size);
    }
    // Production WriteMemory drops the GPU data of the range and marks it CPU-modified
    void WriteMemory(DAddr addr, u64 size) {
        for (u64 page = addr / Page; page < (addr + size) / Page; ++page) {
            PageState& state = world->pages[page];
            if (state.gpu_data) {
                ++world->metrics.lost_gpu_pages;
            }
            if (state.aliased) {
                ++world->metrics.lost_alias_pages;
            }
            state.buffer = false;
            state.gpu_data = false;
        }
    }

    u64 WriteMemoryAhead(DAddr device_addr, u64 size);
};

#include "write_memory_ahead.inc"

namespace {

enum class Policy { None, Current };

const char* Name(Policy policy) {
    switch (policy) {
    case Policy::None:
        return "none";
    case Policy::Current:
        return "current";
    }
    return "";
}

class Simulator {
public:
    Simulator(Policy policy_, std::vector<PageState> pages) : policy{policy_} {
        world.written.assign(pages.size(), false);
        world.ahead.assign(pages.size(), false);
        world.pages = std::move(pages);
    }

    void Write(u64 page) {
        if (world.pages[page].Protected()) {
            Fault(page);
        }
        Check(!world.pages[page].Protected(), "a handled fault leaves the page writable");
        world.written[page] = true;
    }

    Metrics Finish() {
        for (u64 page = 0; page < world.pages.size(); ++page) {
            if (world.ahead[page] && !world.written[page]) {
                ++world.metrics.wasted_pages;
            }
        }
        return world.metrics;
    }

private:
    bool IsProtectedAddr(u64 addr) const {
        const u64 page = addr / Page;
        return page >= world.pages.size() || world.pages[page].Protected();
    }

    // The CPU side window: contiguous protected pages after the fault (memory.cpp)
    u64 WindowPages(u64 page, u64 window_pages) const {
        u64 pages = 1;
        while (pages < window_pages && page + pages < world.pages.size() &&
               world.pages[page + pages].Protected()) {
            ++pages;
        }
        return pages;
    }

    // OnCacheInvalidation: the guest writes this page, every cache drops it
    void FullInvalidate(u64 page, bool ahead) {
        PageState& state = world.pages[page];
        if (ahead) {
            world.ahead[page] = true;
            ++world.metrics.ahead_pages;
            world.metrics.lost_gpu_pages += state.gpu_data;
            world.metrics.lost_texture_pages += state.texture;
        }
        state = {};
    }

    void Fault(u64 page) {
        ++world.metrics.faults;
        const u64 addr = page * Page;
        switch (policy) {
        case Policy::None:
            FullInvalidate(page, false);
            break;
        case Policy::Current: {
            const auto fault = streams.Begin(addr);
            const u64 window = WindowPages(page, fault.window_pages);
            FullInvalidate(page, false);
            // Model VideoManager::InvalidateNceMemory: stop the contiguous candidate range before
            // any following page that has a second device mapping (alias).
            u64 unaliased_ahead = 0;
            while (1 + unaliased_ahead < window && !world.pages[page + 1 + unaliased_ahead].aliased) {
                ++unaliased_ahead;
            }
            u64 ahead = 0;
            if (unaliased_ahead > 0) {
                ahead = cache.WriteMemoryAhead(addr + Page, unaliased_ahead * Page);
                Check(ahead % Page == 0 && ahead <= unaliased_ahead * Page,
                      "WriteMemoryAhead returns whole pages of the range");
                for (u64 i = 1; i <= ahead / Page; ++i) {
                    world.ahead[page + i] = true;
                    ++world.metrics.ahead_pages;
                }
            }
            const u64 invalidated = Page + ahead;
            const u64 next = Core::NextProtectedPage(
                addr + Page, addr + invalidated + MaxWritablePages * Page, Page,
                [&](u64 candidate) { return IsProtectedAddr(candidate); });
            streams.End(fault, next);
            break;
        }
        }
    }

    Policy policy;
    World world;
    BufferCache<void> cache{&world};
    Core::NceWriteStreams<4> streams{};
};

struct Scenario {
    const char* name;
    std::vector<PageState> pages;
    std::vector<u64> writes;
};

std::vector<PageState> Buffer(u64 count) {
    return std::vector<PageState>(count, PageState{.buffer = true});
}

std::vector<u64> Sequence(u64 begin, u64 end) {
    std::vector<u64> pages;
    for (u64 page = begin; page < end; ++page) {
        pages.push_back(page);
    }
    return pages;
}

std::vector<Scenario> Scenarios() {
    std::vector<Scenario> scenarios;

    scenarios.push_back({"sequential 1 MiB buffer", Buffer(256), Sequence(0, 256)});

    {
        // Vertices, indices and uniforms filled in turn, apart in memory
        std::vector<PageState> pages(3 * 128);
        for (u64 stream = 0; stream < 3; ++stream) {
            for (u64 page = 0; page < 64; ++page) {
                pages[stream * 128 + page].buffer = true;
            }
        }
        std::vector<u64> writes;
        for (u64 page = 0; page < 64; ++page) {
            for (u64 stream = 0; stream < 3; ++stream) {
                writes.push_back(stream * 128 + page);
            }
        }
        scenarios.push_back({"3 interleaved buffers", std::move(pages), std::move(writes)});
    }

    {
        auto pages = Buffer(256);
        for (u64 page = 8; page < 256; page += 16) {
            pages[page].texture = true;
        }
        scenarios.push_back({"buffer sharing pages with textures", std::move(pages),
                             Sequence(0, 256)});
    }

    {
        auto pages = Buffer(256);
        for (u64 page = 64; page < 96; ++page) {
            pages[page].buffer = false;
        }
        scenarios.push_back({"buffer partly writable already", std::move(pages),
                             Sequence(0, 256)});
    }

    {
        auto pages = Buffer(64);
        for (u64 page = 34; page < 64; ++page) {
            pages[page].gpu_data = true;
        }
        scenarios.push_back({"GPU data after the written pages", std::move(pages),
                             Sequence(0, 32)});
    }

    {
        auto pages = Buffer(32);
        pages.resize(64, PageState{.texture = true});
        scenarios.push_back({"textures right after a buffer", std::move(pages), Sequence(0, 32)});
    }

    {
        // Only the second page of a coalesced window has a second GPU device mapping (alias).
        // Page 0 faults (1 page), page 1 faults (4-page window), and page 2 has an alias so
        // coalescing must stop before page 2 and leave page 2 to fault on its own full path.
        auto pages = Buffer(32);
        pages[2].aliased = true;
        scenarios.push_back({"second page has an alias", std::move(pages), Sequence(0, 32)});
    }

    {
        std::vector<u64> writes;
        u64 seed = 0x2545F4914F6CDD1DULL;
        for (int i = 0; i < 4096; ++i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            writes.push_back((seed >> 33) % 1024);
        }
        scenarios.push_back({"random pages", Buffer(1024), std::move(writes)});
    }
    return scenarios;
}

Metrics Run(const Scenario& scenario, Policy policy) {
    Simulator simulator{policy, scenario.pages};
    for (const u64 page : scenario.writes) {
        simulator.Write(page);
    }
    return simulator.Finish();
}

void TestStreams() {
    Core::NceWriteStreams<2> streams{};
    auto fault = streams.Begin(0x1000);
    Check(fault.window_pages == 1, "a new stream does not coalesce");
    streams.End(fault, 0x2000);
    fault = streams.Begin(0x2000);
    Check(fault.window_pages == 4, "the second fault of a stream coalesces 4 pages");
    streams.End(fault, 0x6000);
    fault = streams.Begin(0x6000);
    Check(fault.window_pages == 16, "later faults coalesce 16 pages");
    streams.End(fault, 0x16000);

    // New streams take the slots in turn: one other stream keeps the first one alive
    auto other = streams.Begin(0x100000);
    streams.End(other, 0x101000);
    fault = streams.Begin(0x16000);
    Check(fault.window_pages == 16, "a stream survives another stream");
    streams.End(fault, 0x26000);
    auto third = streams.Begin(0x200000);
    streams.End(third, 0x201000);
    Check(streams.Begin(0x26000).window_pages == 1, "a third stream replaced the first one");
    Check(streams.Begin(0x101000).window_pages == 1, "and then the second one");

    Check(Core::NextProtectedPage(0x1000, 0x5000, 0x1000, [](u64 page) { return page == 0x3000; }) ==
              0x3000,
          "the first protected page is found");
    Check(Core::NextProtectedPage(0x1000, 0x5000, 0x1000, [](u64) { return false; }) == 0x5000,
          "no protected page gives the end of the scan");
}

void TestScenarios() {
    std::printf("%-36s %-14s %7s %7s %7s %9s %9s %10s\n", "scenario", "policy", "faults", "ahead",
                "wasted", "lost GPU", "lost tex", "lost alias");
    for (const Scenario& scenario : Scenarios()) {
        const Metrics none = Run(scenario, Policy::None);
        const Metrics current = Run(scenario, Policy::Current);
        for (const auto& [policy, metrics] :
             {std::pair{Policy::None, none}, std::pair{Policy::Current, current}}) {
            std::printf("%-36s %-14s %7llu %7llu %7llu %9llu %9llu %10llu\n", scenario.name, Name(policy),
                        static_cast<unsigned long long>(metrics.faults),
                        static_cast<unsigned long long>(metrics.ahead_pages),
                        static_cast<unsigned long long>(metrics.wasted_pages),
                        static_cast<unsigned long long>(metrics.lost_gpu_pages),
                        static_cast<unsigned long long>(metrics.lost_texture_pages),
                        static_cast<unsigned long long>(metrics.lost_alias_pages));
        }
        Check(current.lost_gpu_pages == 0, "no GPU-written data is dropped ahead of the guest");
        Check(current.lost_texture_pages == 0, "no other cache is dropped ahead of the guest");
        Check(current.lost_alias_pages == 0, "no aliased mapping is skipped ahead of the guest");
        Check(current.faults <= none.faults, "coalescing never adds faults");
    }
}

void TestCases() {
    const auto scenarios = Scenarios();
    const auto find = [&](const char* name) -> const Scenario& {
        for (const Scenario& scenario : scenarios) {
            if (std::string_view{scenario.name} == name) {
                return scenario;
            }
        }
        throw std::runtime_error(name);
    };
    const auto faults = [&](const char* name, Policy policy) {
        return Run(find(name), policy).faults;
    };

    Check(faults("sequential 1 MiB buffer", Policy::Current) <= 20,
          "a sequential 1 MiB fill faults about once per 64 KiB");
    Check(faults("3 interleaved buffers", Policy::Current) <= 24,
          "interleaved buffers keep their own streams");
    // 16 pages hold a texture: each faults once, as it must, and the stream goes on
    Check(faults("buffer sharing pages with textures", Policy::Current) <=
              faults("sequential 1 MiB buffer", Policy::Current) + 16,
          "a texture page faults alone without restarting the stream");
    Check(faults("buffer partly writable already", Policy::Current) <= 20,
          "writable pages do not restart the stream");
    Check(Run(find("second page has an alias"), Policy::Current).lost_alias_pages == 0,
          "coalescing stops before an aliased second page");
    Check(Run(find("random pages"), Policy::Current).wasted_pages * 10 <
              Run(find("random pages"), Policy::Current).faults,
          "random writes barely invalidate ahead");
}

} // namespace

int main() {
    try {
        TestStreams();
        TestScenarios();
        TestCases();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("nce_write_streams: %u checks passed\n", checks);
    return 0;
}
