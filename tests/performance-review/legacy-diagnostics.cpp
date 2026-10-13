// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>
#include "yuzu_video_core/performance_diagnostics.h"

namespace Perf = VideoCommon::PerformanceDiagnostics;

int main() {
#if NXEMU_ENABLE_LEGACY_PERFORMANCE_DIAGNOSTICS
    _putenv_s("NXEMU_PERFORMANCE_DIAGNOSTICS", "1");
    if (!Perf::Enabled()) return 1;
#else
    // Runtime environment cannot accidentally reactivate the retired hot-path timers.
    _putenv_s("NXEMU_PERFORMANCE_DIAGNOSTICS", "1");
    static_assert(!Perf::Enabled());
    static_assert(std::is_empty_v<Perf::Scope>);
    static_assert(std::is_trivially_destructible_v<Perf::Scope>);
#endif
    int calls{}, value=7;
    const int result=Perf::Measure(Perf::Category::Draw,[&] {
        return Perf::Measure(Perf::Category::GraphicsViews,[&] {++calls;return 19;});
    });
    Perf::Measure(Perf::Category::GuestFlush,[&] {++calls;});
    int& alias=Perf::Measure(Perf::Category::GuestFlush,[&]() -> int& {++calls;return value;});
    alias=23;
    try {
        Perf::Measure(Perf::Category::Draw,[&] {
            Perf::Measure(Perf::Category::GraphicsViews,[&] {++calls;throw 29;});
        });
        return 2;
    } catch (int error) {if(error!=29) return 3;}
    if(calls!=4||result!=19||value!=23) return 4;
    Perf::RecordEvent(Perf::Category::ReadbackCopy,31);
    Perf::RecordBytes(Perf::Category::ReadbackCopy,37);
    const auto& state=Perf::thread_counters;
    if(state.depth!=0) return 5;
#if NXEMU_ENABLE_LEGACY_PERFORMANCE_DIAGNOSTICS
    for(auto category:{Perf::Category::Draw,Perf::Category::GraphicsViews,Perf::Category::GuestFlush}) {
        if(state.counters[static_cast<size_t>(category)].calls!=2) return 6;
    }
    const auto& counter=state.counters[static_cast<size_t>(Perf::Category::ReadbackCopy)];
    if(counter.calls!=1||counter.bytes!=68) return 7;
    bool child_ok{};
    std::thread child([&] {
        {Perf::Scope scope{Perf::Category::Draw};}
        child_ok=Perf::thread_counters.depth==0 &&
                 Perf::thread_counters.counters[static_cast<size_t>(Perf::Category::Draw)].calls==1;
    });
    child.join();
    if(!child_ok||state.counters[static_cast<size_t>(Perf::Category::Draw)].calls!=2) return 8;
#else
    for(const auto& counter:state.counters) {
        if(counter.calls!=0||counter.bytes!=0||counter.elapsed!=Perf::Clock::duration::zero()) return 9;
    }
#endif
    std::cout<<"PASS legacy_diagnostics="<<NXEMU_ENABLE_LEGACY_PERFORMANCE_DIAGNOSTICS
             <<" void/reference forwarding, exception unwinding and counters\n";
}
