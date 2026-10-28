// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>
#include <unordered_set>

namespace VideoCommon::ShaderProfile {
// Accounted inputs/proofs only; native Vulkan driver allocations are separate.
class CandidateWarmupBudget {
public:
    static constexpr size_t MaxBytes = 256 * 1024 * 1024ULL;
    static constexpr size_t DefaultCandidates = 2048;
    static constexpr size_t MaxCandidates = 16384;
    static constexpr size_t MaxReplacementQueries = 4096;
    static constexpr size_t BaseProofUnits = 20;
    static constexpr size_t MaxTextureProofQueries = 16;
    static constexpr size_t MaxStrictTextureReads = 64;
    static constexpr size_t MaxSemanticProofUnits = MaxTextureProofQueries + MaxStrictTextureReads;
    static constexpr size_t ProofUnitBytes = 64;

    static size_t ParseLimit(std::string_view value) noexcept {
        if(value.empty())return DefaultCandidates;
        size_t limit{};
        const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),limit);
        return error==std::errc{} && end==value.data()+value.size() && limit<=MaxCandidates ?
            limit : DefaultCandidates;
    }
    explicit CandidateWarmupBudget(size_t limit=DefaultCandidates) noexcept :
        candidate_limit(std::min(limit,MaxCandidates)) {}
    size_t Reserve(size_t stages,bool hle,size_t retained) noexcept {
        if(!stages || stages>6 || accepted>=candidate_limit || retained>MaxBytes) {
            ++skipped;return 0;
        }
        const auto requested=stages*(BaseProofUnits+MaxSemanticProofUnits+(hle?MaxReplacementQueries:0))*ProofUnitBytes;
        if(requested>MaxBytes-retained || reserved>MaxBytes-retained-requested) {
            ++skipped;return 0;
        }
        reserved+=requested;++accepted;return requested;
    }
    bool Release(size_t reservation) noexcept {
        if(!reservation || reservation>reserved)return false;
        reserved-=reservation;return true;
    }
    // Caller holds the startup mutex, as for Reserve/Release.
    bool Commit(size_t reservation,size_t proof,size_t& retained) noexcept {
        if(!reservation || reservation>reserved || proof>reservation || retained>MaxBytes ||
           reserved>MaxBytes-retained)return false;
        reserved-=reservation;retained+=proof;return true;
    }
    [[nodiscard]] size_t Accepted() const noexcept {return accepted;}
    [[nodiscard]] size_t Skipped() const noexcept {return skipped;}
    [[nodiscard]] size_t Reserved() const noexcept {return reserved;}
private:
    size_t accepted{},skipped{},reserved{},candidate_limit;
};

struct StartupInput {
    const void* identity;
    size_t total,code;
};
// One atomic admission per pipeline. A rejected multi-stage pipeline must not
// consume bytes or mark a program as already accounted for a later admission.
inline bool AccountStartupInputs(std::span<const StartupInput> inputs,
                                std::unordered_set<const void*>& owners,size_t& retained) {
    constexpr auto limit=CandidateWarmupBudget::MaxBytes;
    if(inputs.empty() || inputs.size()>6 || retained>limit)return false;
    std::array<const void*,6> added{};
    size_t count{},bytes{};
    for(const auto& input:inputs) {
        if(!input.identity || input.code>input.total)return false;
        const bool shared=owners.contains(input.identity) ||
            std::find(added.begin(),added.begin()+count,input.identity)!=added.begin()+count;
        const auto amount=input.total-(shared?input.code:0);
        if(amount>limit-bytes)return false;
        bytes+=amount;
        if(!shared)added[count++]=input.identity;
    }
    if(bytes>limit-retained)return false;
    size_t inserted{};
    try {
        for(;inserted<count;++inserted)owners.insert(added[inserted]);
    } catch(...) {
        // Optional callers may continue after allocation failure. Do not leave
        // uncharged ownership entries that would undercount the next pipeline.
        for(size_t i=0;i<inserted;++i)owners.erase(added[i]);
        throw;
    }
    retained+=bytes;
    return true;
}
} // namespace VideoCommon::ShaderProfile
