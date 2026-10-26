// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_metrics.h"
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace VideoCommon::ShaderProfile::Timing;
void require(bool value) { if(!value) throw std::runtime_error("timing regression"); }
int main() {
    Metrics metrics;
    require(Measure(nullptr,Phase::ProfileIO,[] { return std::make_unique<int>(7); }) && !current);
    {
        Context root(&metrics);
        { Context disabled(nullptr); require(!current); }
        require(current==&metrics);
    }
    require(!current);
    std::vector<std::thread> workers;
    for(int t=0;t<4;++t) workers.emplace_back([&] {
        require(!current);
        Context context(&metrics);
        for(int i=0;i<50;++i) Measure(current,Phase::GuestTranslation,[&] {
            metrics.Add(Counter::Programs,1);
        });
    });
    for(auto& worker:workers) worker.join();
    const auto translation=static_cast<size_t>(Phase::GuestTranslation);
    require(metrics.calls[translation]==200 && metrics.completed[translation]==200);
    require(metrics.counts[static_cast<size_t>(Counter::Programs)]==200);
    try { Measure(&metrics,Phase::NativeGraphics,[] { throw std::runtime_error("expected"); }); }
    catch(const std::runtime_error&) {}
    const auto native=static_cast<size_t>(Phase::NativeGraphics);
    require(metrics.calls[native]==1 && metrics.completed[native]==0);
    { Timer once(&metrics,Phase::ProfileIO); once.Stop(); once.Stop(); }
    require(metrics.calls[static_cast<size_t>(Phase::ProfileIO)]==1);
    require(metrics.Json().find("\"schema\":1")!=std::string::npos);
}
