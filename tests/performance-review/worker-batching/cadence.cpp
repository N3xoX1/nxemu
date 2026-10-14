// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>
using u32=unsigned;
u32 draw_index{};
struct Scheduler {
    std::vector<u32> dispatches,submissions;
    void DispatchWork(){dispatches.push_back(draw_index);}
    void Flush(){submissions.push_back(draw_index);}
};
#include "baseline.inc"
#include "candidate.inc"
int main(){
    unsigned checks{};
    for(unsigned scenario=0;scenario<32;++scenario){
        baseline::RasterizerVulkan before;
        candidate::RasterizerVulkan after;
        std::mt19937 random(0x295396+scenario);
        for(draw_index=1;draw_index<=100000;++draw_index){
            // External flushes reset draw_counter in production. Force short,
            // irregular draw sequences as well as sustained periodic submission.
            if(scenario && random()%1024<scenario){
                before.scheduler.Flush();after.scheduler.Flush();
                before.draw_counter=0;after.draw_counter=0;
            }
            before.FlushWork();after.FlushWork();
            if(before.draw_counter!=after.draw_counter)return 1;
            ++checks;
        }
        if(before.scheduler.submissions!=after.scheduler.submissions)return 2;
        for(u32 draw:after.scheduler.dispatches){
            // Every new opportunistic handoff must be an original handoff.
            if(!std::binary_search(before.scheduler.dispatches.begin(),before.scheduler.dispatches.end(),draw))return 3;
        }
        if(scenario==0 && after.scheduler.dispatches.size()*3>=before.scheduler.dispatches.size())return 4;
    }
    std::printf("PASS %u draw/reset cases: exact GPU submission indices and counter state, only opportunistic handoffs reduced\n",checks);
}
