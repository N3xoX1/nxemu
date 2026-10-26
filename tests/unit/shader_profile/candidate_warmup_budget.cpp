// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_candidate_budget.h"
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace VideoCommon::ShaderProfile;
void Check(bool ok) {if(!ok)throw std::runtime_error("Candidate admission regression");}
int main() try {
    constexpr auto max=CandidateWarmupBudget::MaxBytes;
    for(auto invalid:{"","-1","+0"," 128","16385","20x","999999999999999999999999"})
        Check(CandidateWarmupBudget::ParseLimit(invalid)==2048);
    for(size_t limit:{0,128,512,2048,8192})
        Check(CandidateWarmupBudget::ParseLimit(std::to_string(limit))==limit);
    CandidateWarmupBudget budget;
    constexpr size_t proof=(CandidateWarmupBudget::BaseProofUnits+CandidateWarmupBudget::MaxSemanticProofUnits)*CandidateWarmupBudget::ProofUnitBytes;
    size_t retained=max-proof;
    auto ticket=budget.Reserve(1,false,retained);
    Check(ticket==proof && !budget.Reserve(1,false,retained));
    Check(!budget.Commit(ticket,ticket+1,retained));
    Check(budget.Reserved()==ticket && retained==max-proof);
    Check(budget.Commit(ticket,640,retained));
    Check(retained==max-proof+640 && budget.Reserved()==0);
    Check(!budget.Release(ticket));
    Check(!budget.Reserve(0,false,0) && !budget.Reserve(7,true,0));
    Check(!budget.Reserve(1,false,max+1));
    ticket=budget.Reserve(6,true,0);
    Check(ticket==6*(CandidateWarmupBudget::MaxReplacementQueries*CandidateWarmupBudget::ProofUnitBytes+proof));
    Check(budget.Release(ticket) && budget.Reserved()==0);
    std::unordered_set<const void*> owners;
    int a{},b{};
    retained=max-200;
    const StartupInput too_big[]{{&a,100,80},{&b,200,100}};
    Check(!AccountStartupInputs(too_big,owners,retained));
    Check(owners.empty() && retained==max-200);
    const StartupInput first[]{{&a,100,80}};
    Check(AccountStartupInputs(first,owners,retained) && retained==max-100);
    const StartupInput repeat[]{{&a,100,80},{&a,100,80}};
    Check(AccountStartupInputs(repeat,owners,retained) && retained==max-60);
    Check(owners.size()==1);
    const StartupInput invalid[]{{&b,(std::numeric_limits<size_t>::max)(),100}};
    Check(!AccountStartupInputs(invalid,owners,retained) && owners.size()==1);
    const StartupInput bad_size[]{{&b,10,11}};
    Check(!AccountStartupInputs(bad_size,owners,retained));
    retained=max+1;
    Check(!AccountStartupInputs(first,owners,retained));
    // Concurrent workers finish reservations under the renderer's startup mutex.
    CandidateWarmupBudget concurrent(8192);
    retained=0;std::mutex mutex;
    std::vector<std::thread> workers;
    for(unsigned thread=0;thread<8;++thread)workers.emplace_back([&] {
        for(unsigned i=0;i<1024;++i) {
            size_t reservation;
            {std::scoped_lock lock(mutex);reservation=concurrent.Reserve(1,true,retained);}
            Check(reservation!=0);
            {std::scoped_lock lock(mutex);Check(concurrent.Commit(reservation,1280,retained));}
        }
    });
    for(auto& worker:workers)worker.join();
    Check(concurrent.Accepted()==8192 && concurrent.Reserved()==0 && retained==8192*1280);
    Check(!concurrent.Reserve(1,false,retained));
    std::cout<<"{\"scope\":\"synthetic_admission_only_no_Vulkan\",\"limits\":[";
    bool comma{};
    for(size_t limit:{0,128,512,2048,8192}) {
        CandidateWarmupBudget candidate(limit);
        size_t bytes=64*1024*1024;
        for(unsigned i=0;i<10000;++i) {
            const auto reservation=candidate.Reserve(2,true,bytes);
            if(reservation)Check(candidate.Commit(reservation,2560,bytes));
        }
        Check(candidate.Accepted()==limit && candidate.Skipped()==10000-limit);
        Check(bytes==64*1024*1024+limit*2560 && candidate.Reserved()==0);
        if(comma)std::cout<<',';comma=true;
        std::cout<<"{\"limit\":"<<limit<<",\"accepted\":"<<candidate.Accepted()
                 <<",\"skipped\":"<<candidate.Skipped()<<",\"accounted_bytes\":"<<bytes
                 <<'}';
    }
    std::cout<<"]}\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
