// SPDX-License-Identifier: GPL-2.0-or-later
// Executes the production admission state machine and native queue. Compilation
// work is controlled CPU-only work; Vulkan/Metal creation is tested separately.
#include "yuzu_video_core/renderer_vulkan/progressive_pipeline_warmup.h"
#include <future>
#include <iostream>
#include <semaphore>
#include <stdexcept>

// Only OS thread naming is stubbed; scheduling and synchronization are real.
namespace Common { void SetCurrentThreadName(const char*) {} }
using Vulkan::ProgressivePipelineWarmup;
using Result = ProgressivePipelineWarmup::DemandResult;
using namespace std::chrono_literals;
static void Check(bool result) {
    if (!result) throw std::runtime_error("Progressive warmup regression");
}

int main() try {
    static_assert(Vulkan::InitialWarmupBudget::InitialCount==512);
    static_assert(Vulkan::InitialWarmupBudget::InitialMilliseconds==60000);
    for (const char* option : {static_cast<const char*>(nullptr), "", "0", "true", "2", "01"})
        Check(!Vulkan::ProgressiveWarmupEnabled(option));
    Check(Vulkan::ProgressiveWarmupEnabled("1"));
    for (const bool enabled : {false, true}) {
        Vulkan::InitialWarmupBudget budget(enabled,256,0);
        for (size_t i = 0; i < 1000; ++i) Check(budget.DeferNext() == (enabled && i >= 256));
        Check(budget.Scheduled() == (enabled ? 256 : 1000));
    }
    // Production budget: at least N, then continue up to the soft duration.
    {
        using Budget=Vulkan::InitialWarmupBudget;
        const auto start=Budget::Clock::time_point{}+1s;
        Budget defaults(true);
        for(size_t i=0;i<512;++i)Check(!defaults.DeferNext(start));
        Check(!defaults.DeferNext(start+59s));
        Check(defaults.DeferNext(start+60s) && defaults.Scheduled()==513);
        Budget fast(true,256,60000);
        for(size_t i=0;i<512;++i)Check(!fast.DeferNext(start+std::chrono::milliseconds(i)));
        Check(fast.DeferNext(start+60s));
        Check(fast.Scheduled()==512 && fast.DeferNext(start+61s));
        Budget slow(true,256,60000);
        for(size_t i=0;i<256;++i)Check(!slow.DeferNext(start+std::chrono::seconds(i)));
        Check(slow.NeedsCheckpoint(4) && slow.DeferNext(start+256s));
        Budget larger(true,512,30000);
        for(size_t i=0;i<512;++i)Check(!larger.DeferNext(start+std::chrono::seconds(i)));
        Check(larger.DeferNext(start+512s));
        for(const char* bad : {"-1","60001","1x"," 100","18446744073709551616"})
            Check(Budget::ParseOption(bad,60000,60000)==60000);
        Check(Budget::ParseOption("30000",60000,60000)==30000);
        Check(Budget::ParseOption("0",60000,60000)==0);
    }
    // Stable admission order, single speculative build and one publication each.
    for (const size_t workers : {1, 4}) {
        Vulkan::NativePipelineBuildQueue queue(workers);
        std::vector<size_t> order, published;
        std::binary_semaphore done{0};
        std::atomic<size_t> active{}, max_active{};
        ProgressivePipelineWarmup warmup(queue,4096,[&](size_t i) {
            const auto count=++active;
            if(count>max_active)max_active=count;
            order.push_back(i);
            --active;
            if(i==4095)done.release();
            return true;
        });
        done.acquire();
        Check(warmup.Demand(4095,false)==Result::Complete);
        warmup.Stop();queue.WaitForRequests();
        warmup.PublishReady([&](size_t i){published.push_back(i);});
        warmup.PublishReady([](size_t){Check(false);});
        Check(max_active==1 && order==published && order.size()==4096);
        for(size_t i=0;i<order.size();++i)Check(order[i]==i);
    }
    // A requested queued entry is withdrawn before admission; a running entry
    // is promoted/skipped for async draws and waited for urgent/compute use.
    {
        Vulkan::NativePipelineBuildQueue queue(2);
        std::binary_semaphore entered{0},release{0},last{0};
        std::vector<size_t> built;
        ProgressivePipelineWarmup warmup(queue,3,[&](size_t i) {
            built.push_back(i);
            if(i==0){entered.release();release.acquire();}
            if(i==2)last.release();
            return true;
        });
        entered.acquire();
        Check(warmup.Demand(1,false)==Result::Runtime);
        Check(warmup.Demand(0,true)==Result::Pending);
        auto urgent=std::async(std::launch::async,[&]{return warmup.Demand(0,false);});
        Check(urgent.wait_for(10ms)==std::future_status::timeout);
        release.release();
        Check(urgent.get()==Result::Complete);
        last.acquire();Check(warmup.Demand(2,false)==Result::Complete);
        warmup.Stop();queue.WaitForRequests();
        Check(built==std::vector<size_t>({0,2}));
        Check(warmup.Demand(1,true)==Result::Runtime);
    }
    // Stopping before any work is admitted must be safe too.
    {
        Vulkan::NativePipelineBuildQueue queue(1);
        ProgressivePipelineWarmup warmup(queue,0,[](size_t){Check(false);return false;});
        warmup.Stop();
    }
    // Destruction while a native build is in flight must wait for its input
    // access to end, and must not admit the next job.
    {
        Vulkan::NativePipelineBuildQueue queue(1);
        std::binary_semaphore entered{0},release{0};
        std::atomic<size_t> builds{};
        auto warmup=std::make_unique<ProgressivePipelineWarmup>(queue,10,[&](size_t) {
            ++builds;entered.release();release.acquire();return true;
        });
        entered.acquire();warmup->Stop();
        auto destroy=std::async(std::launch::async,[&]{warmup.reset();});
        Check(destroy.wait_for(10ms)==std::future_status::timeout);
        release.release();destroy.get();Check(builds==1);
    }
    // Failed and throwing compiler callbacks leave a retryable runtime key.
    {
        Vulkan::NativePipelineBuildQueue queue(1);
        std::binary_semaphore done{0};
        ProgressivePipelineWarmup warmup(queue,3,[&](size_t i) {
            if(i==0)throw std::runtime_error("compiler failure");
            if(i==2)done.release();
            return i==2;
        });
        done.acquire();Check(warmup.Demand(2,false)==Result::Complete);
        Check(warmup.Demand(0,false)==Result::Runtime && warmup.Demand(1,false)==Result::Runtime);
        warmup.Stop();queue.WaitForRequests();
        size_t published{};
        warmup.PublishReady([&](size_t i){Check(i==2);++published;});Check(published==1);
    }
    // A slow frame delays admission, but never blocks ordinary urgent work.
    {
        Vulkan::NativePipelineBuildQueue queue(1);
        std::binary_semaphore entered{0},release{0},second{0};
        ProgressivePipelineWarmup warmup(queue,2,[&](size_t i) {
            if(i==0){entered.release();release.acquire();}else second.release();
            return true;
        });
        entered.acquire();
        const auto now=ProgressivePipelineWarmup::Clock::now();
        warmup.RecordFrame(now-50ms);warmup.RecordFrame(now);
        release.release();Check(warmup.Demand(0,false)==Result::Complete);
        Check(!second.try_acquire_for(20ms));
        Check(warmup.Demand(1,false)==Result::Runtime);
        std::binary_semaphore urgent{0};queue.QueueWork([&]{urgent.release();},true);
        urgent.acquire();warmup.Stop();queue.WaitForRequests();
        Check(!second.try_acquire());
    }
    // Ready-result publication can recover after the owner's callback throws.
    {
        Vulkan::NativePipelineBuildQueue queue(1);
        std::binary_semaphore done{0};
        ProgressivePipelineWarmup warmup(queue,1,[&](size_t){done.release();return true;});
        done.acquire();Check(warmup.Demand(0,false)==Result::Complete);
        warmup.Stop();queue.WaitForRequests();
        bool caught{};
        try{warmup.PublishReady([](size_t){throw std::runtime_error("publish failure");});}
        catch(const std::runtime_error&){caught=true;}
        Check(caught);
        size_t count{};warmup.PublishReady([&](size_t){++count;});Check(count==1);
    }
    std::cout << "PASS: production progressive admission, urgent demand, failures, pause and shutdown\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
