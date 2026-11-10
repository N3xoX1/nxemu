// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_shader_recompiler/environment.h"
#include <algorithm>
#include "yuzu_video_core/shader_profile_sph.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_candidate_budget.h"
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VideoCommon::ShaderProfile {
// Runtime proof only. Hash each immutable target program once, independently
// of pipeline count. Candidate guest code can be released after preparation.
class ProgramEquivalence {
    using Program=std::shared_ptr<const std::vector<u64>>;
    using Owner=std::weak_ptr<const std::vector<u64>>;
    std::map<Owner,std::array<u8,8>,std::owner_less<>> relocation;
    std::map<Owner,std::string,std::owner_less<>> digests;
public:
    bool MatchDigest(std::string_view expected,size_t bytes,const Program& target) {
        if(!target || expected.size()!=64 || target->size()*sizeof(u64)!=bytes)return false;
        auto found=digests.find(Owner(target));
        if(found==digests.end()) {
            if(digests.size()>=16384)digests.clear();
            found=digests.emplace(Owner(target),ResourceSha256(
                {reinterpret_cast<const char*>(target->data()),bytes})).first;
        }
        return expected==found->second;
    }
    bool CanRebase(const Program& program,Shader::Stage stage,u32 source,u32 target) {
        if(!program || program->empty() || (stage!=Shader::Stage::Compute && program->size()<10) || source%8 || source%32!=target%32 ||
           program->size()*8>UINT32_MAX-source || program->size()*8>UINT32_MAX-target)return false;
        if(relocation.size()>=16384)relocation.clear();
        auto& facts=relocation[Owner(program)];
        const size_t key=(stage==Shader::Stage::Compute?0:4)+(source%32)/8;
        if(!facts[key]) {
            bool safe=true;
            // Scheduling words are aligned in GPU program address space. Reject
            // absolute JMX/JMP/JCAL and PC reads (Maxwell maxwell.inc/CFG). All
            // remaining supported control flow is relative to the instruction.
            for(size_t i=stage==Shader::Stage::Compute?0:10;i<program->size();++i) {
                if((u64{source}+i*8)%32==0)continue;
                const auto opcode=(*program)[i]>>52;
                if(opcode==0xe20 || opcode==0xe21 || opcode==0xe22 || opcode==0x50d){safe=false;break;}
            }
            facts[key]=safe?1:2;
        }
        return facts[key]==1;
    }

};
// Learned locally while translating a foreign candidate, never shared or serialized.
// Overflow disables relaxation; the original exact checks remain the fallback.
struct SemanticTextureProof {
    struct Query {
        Shader::TextureHandleSource source;
        Shader::TextureQueryKind kind;
        u32 old_handle, answer;
    };
    std::vector<Query> queries;
    std::vector<std::pair<unsigned,u64>> strict_reads;
    bool complete{true}, in_query{};
    void Strict(unsigned kind,u64 key) {
        if(in_query || !complete)return;
        const auto value=std::pair{kind,key};
        if(std::find(strict_reads.begin(),strict_reads.end(),value)!=strict_reads.end())return;
        if(strict_reads.size()>=CandidateWarmupBudget::MaxStrictTextureReads){complete=false;return;}
        strict_reads.push_back(value);
    }
    void Add(Query query) {
        if(!complete)return;
        for(const auto& previous:queries)
            if(previous.source==query.source && previous.kind==query.kind) {
                if(previous.old_handle!=query.old_handle || previous.answer!=query.answer)complete=false;
                return;
            }
        if(queries.size()>=CandidateWarmupBudget::MaxTextureProofQueries){complete=false;return;}
        queries.push_back(std::move(query));
    }
    bool StrictlyRead(unsigned kind,u64 key) const {
        return std::find(strict_reads.begin(),strict_reads.end(),std::pair{kind,key})!=strict_reads.end();
    }
    bool OnlyTextureConstant(u64 key) const {
        if(!complete || StrictlyRead(0,key))return false;
        for(const auto& query:queries) {
            const auto& source=query.source;
            if(key==((u64{source.cbuf_index}<<32)|source.cbuf_offset) ||
               (source.has_secondary && key==((u64{source.secondary_cbuf_index}<<32)|source.secondary_cbuf_offset)))return true;
        }
        return false;
    }
    bool OnlyTextureHandle(unsigned kind,u32 handle) const {
        if(!complete || StrictlyRead(kind,handle))return false;
        for(const auto& query:queries)if(query.old_handle==handle &&
            ((kind==1 && query.kind==Shader::TextureQueryKind::Type) ||
             (kind==2 && query.kind!=Shader::TextureQueryKind::Type)))return true;
        return false;
    }
    bool Matches(Shader::Environment& live) const {
        if(!complete)return false;
        const u32 banks=live.ShaderStage()==Shader::Stage::Compute?8U:18U;
        for(const auto& query:queries) {
            const auto& source=query.source;
            if(source.count!=1 || source.shift_left>=32 || source.secondary_shift_left>=32)return false;
            const auto valid=[&](u32 bank,u32 offset){return bank<banks && offset%4==0 && live.ReadCbufSize(bank)!=0;};
            if(!valid(source.cbuf_index,source.cbuf_offset) ||
               (source.has_secondary && !valid(source.secondary_cbuf_index,source.secondary_cbuf_offset)))return false;
            if(live.ReadTextureQuery(source,query.kind)!=query.answer)return false;
        }
        return true;
    }
    size_t Units() const {return queries.size()+strict_reads.size();}
};

struct DependencyProof {
    Shader::Stage stage;
    u32 start,local_memory,shared_memory,texture_bound,viewport;
    std::array<u32,3> workgroup;
    const Shader::ProgramHeader& header;
    const std::array<u32,8>& passthrough;
    const std::unordered_map<u32,Shader::TextureType>& texture_types;
    const std::unordered_map<u32,Shader::TexturePixelFormat>& pixel_formats;
    const std::unordered_map<u64,u32>& constants;
    const std::unordered_map<u64,Shader::ReplaceConstant>& replacements;
    const std::unordered_map<u64,std::optional<Shader::ReplaceConstant>>* replacement_queries{};
    bool complete{},hle{},viewport_queried{true},missing_pool{};
    const SemanticTextureProof* semantic_textures{};
    // Call only after exact immutable program equivalence and matching pipeline
    // state. No reads of unrelated resources, full cbufs or descriptor tables.
    bool Matches(Shader::Environment& live,bool relative_relocation=false) const {
        if(complete) {
            if(hle!=live.HasHLEMacroState() || !replacement_queries)return false;
            for(const auto& [key,value]:*replacement_queries) {
                const u32 bank=static_cast<u32>(key>>32),offset=static_cast<u32>(key);
                if(bank>=18 || offset%4 || live.GetReplaceConstBuffer(bank,offset)!=value)return false;
            }
        } else if(!replacements.empty() || live.HasHLEMacroState())return false;
        if(missing_pool && live.ReadTexturePoolInfo())return false;
        if(stage!=live.ShaderStage() || (!relative_relocation && start!=live.StartAddress()) ||
           local_memory!=live.LocalMemorySize() || shared_memory!=live.SharedMemorySize() ||
           texture_bound!=live.TextureBoundBuffer() || workgroup!=live.WorkgroupSize() ||
           ((!complete || viewport_queried) && viewport!=live.ReadViewportTransformState()))return false;
        if(stage!=Shader::Stage::Compute && CanonicalGuestHeader(header)!=CanonicalGuestHeader(live.SPH()))return false;
        if(stage==Shader::Stage::Geometry && passthrough!=live.GpPassthroughMask())return false;
        const auto* semantic=complete && semantic_textures && semantic_textures->complete &&
            !semantic_textures->queries.empty() ? semantic_textures : nullptr;
        if(semantic && !semantic->Matches(live))return false;
        for(const auto& [handle,type]:texture_types)
            if((!semantic || !semantic->OnlyTextureHandle(1,handle)) && live.ReadTextureType(handle)!=type)return false;
        for(const auto& [handle,format]:pixel_formats)
            if((!semantic || !semantic->OnlyTextureHandle(2,handle)) && live.ReadTexturePixelFormat(handle)!=format)return false;
        std::optional<Shader::TexturePoolInfo> pool;
        for(const auto& [key,value]:constants) {
            if(semantic && semantic->OnlyTextureConstant(key))continue;
            const u32 bank=static_cast<u32>(key>>32),offset=static_cast<u32>(key);
            if(key==UINT64_MAX) {
                if(live.ReadTextureTableSize()!=value)return false;
            } else if(key==UINT64_MAX-1 || key==UINT64_MAX-2) {
                if(!pool)pool=live.ReadTexturePoolInfo();
                if(!pool)return false;
                const auto expected=key==UINT64_MAX-1?pool->num_textures:
                    pool->num_samplers|(pool->linked_samplers?1U<<31:0U);
                if(expected!=value)return false;
            } else {
                if(bank >= (stage==Shader::Stage::Compute?8U:18U))return false;
                if(offset==UINT32_MAX) {
                    if(live.ReadCbufSize(bank)!=value)return false;
                } else {
                    // A changed update may disable this bank; live reads assert enabled.
                    if(offset%4 || !live.ReadCbufSize(bank) || live.ReadCbufValue(bank,offset)!=value)return false;
                }
            }
        }
        return true;
    }
};

// The questions one translation asked of an environment. A synthetic one answers others too: every texture slot
// its shader list names, of which a program reads a few.
struct AskedQuestions {
    // Constant buffer reads the code itself made (not to find a texture handle), sizes, table and pool:
    // answers it may depend on in any way.
    std::unordered_set<u64> constants;
    // Texture queries: where the handle came from, what was asked of it and the answer.
    std::vector<SemanticTextureProof::Query> textures;
    // Texture types and formats asked by handle outside a query.
    std::unordered_set<u32> texture_types, pixel_formats;
    bool viewport{};
    // Translated for an HLE macro draw: every constant buffer load asked whether the draw replaces it, and
    // the answer, none included. Without it the translation never asked, so it proves nothing about them.
    bool hle{};
    std::unordered_map<u64, std::optional<Shader::ReplaceConstant>> replacements;
    void Texture(const SemanticTextureProof::Query& query) {
        for (const auto& previous : textures)
            if (previous.source == query.source && previous.kind == query.kind) return;
        textures.push_back(query);
    }
};
// The live answers to a synthetic environment's questions.
struct SyntheticAnswers {
    std::unordered_map<u64, u32> constants;
    std::unordered_map<u32, Shader::TextureType> texture_types;
    std::unordered_map<u32, Shader::TexturePixelFormat> pixel_formats;
};
// Answers the questions a translation asked of a synthetic (generated) environment from a live draw. A
// constant the code read must be the same; a texture query is asked again where its handle comes from, so a
// generated handle stands for whatever the live draw holds there, and its answer must mean the same to
// translation. Also the draw's own state. Empty, naming the first difference in mismatch, when an answer
// differs in a way the translated code depends on. relocatable: the program runs unchanged at the live start
// address. q holds the environment's answers and fixed facts.
inline std::optional<SyntheticAnswers> AnswerSynthetic(const DependencyProof& q, const AskedQuestions& asked,
                                                       Shader::Environment& live, bool relocatable,
                                                       const char*& mismatch) try {
    const auto differ = [&](const char* why) {
        mismatch = why;
        return std::optional<SyntheticAnswers>{};
    };
    if (live.ShaderStage() != q.stage) return differ("stage");
    if (live.TextureBoundBuffer() != q.texture_bound) return differ("texture bank");
    if (live.LocalMemorySize() != q.local_memory) return differ("local memory");
    if (q.stage == Shader::Stage::Compute
            ? live.SharedMemorySize() != q.shared_memory || live.WorkgroupSize() != q.workgroup
            : CanonicalGuestHeader(q.header) != CanonicalGuestHeader(live.SPH()))
        return differ("program header");
    if (q.stage == Shader::Stage::Geometry && q.passthrough != live.GpPassthroughMask()) return differ("passthrough");
    if (live.StartAddress() != q.start && !relocatable) return differ("start address");
    if (!q.replacements.empty()) return differ("HLE replacement");
    // Stages before rasterization may read the viewport transform; fragment and compute stages never do.
    if (asked.viewport && q.stage != Shader::Stage::Fragment && q.stage != Shader::Stage::Compute &&
        live.ReadViewportTransformState() != q.viewport)
        return differ("viewport transform");
    const u32 banks = q.stage == Shader::Stage::Compute ? 8U : 18U;
    // A live read of a disabled bank asserts.
    const auto readable = [&](u32 bank, u32 offset) { return bank < banks && offset % 4 == 0 && live.ReadCbufSize(bank) != 0; };
    // The draw must replace exactly the loads the translation was told it replaces: none.
    if (asked.hle && !live.HasHLEMacroState()) return differ("HLE draw");
    for (const auto& [key, answer] : asked.replacements) {
        const u32 bank = static_cast<u32>(key >> 32), offset = static_cast<u32>(key);
        if (bank >= banks || offset % 4 || live.GetReplaceConstBuffer(bank, offset) != answer) return differ("HLE replacement");
    }
    SyntheticAnswers answers;
    for (const auto key : asked.constants) {
        const auto found = q.constants.find(key);
        if (found == q.constants.end()) return differ("constant buffer");
        const u32 value = found->second, bank = static_cast<u32>(key >> 32), offset = static_cast<u32>(key);
        if (key == UINT64_MAX) {
            if (live.ReadTextureTableSize() != value) return differ("texture table size");
        } else if (key == UINT64_MAX - 1 || key == UINT64_MAX - 2) {
            const auto pool = live.ReadTexturePoolInfo();
            if (!pool) return differ("texture pool");
            const u32 expected = key == UINT64_MAX - 1 ? pool->num_textures
                                                       : pool->num_samplers | (pool->linked_samplers ? 1U << 31 : 0U);
            if (expected != value) return differ("texture pool");
        } else if (bank >= banks) {
            return differ("constant buffer");
        } else if (offset == UINT32_MAX) {
            if (live.ReadCbufSize(bank) != value) return differ("constant buffer size");
        } else {
            if (!readable(bank, offset)) return differ("constant buffer");
            if (live.HasHLEMacroState() && live.GetReplaceConstBuffer(bank, offset)) return differ("HLE replacement");
            if (live.ReadCbufValue(bank, offset) != value) return differ("constant value");
        }
        answers.constants.emplace(key, value);
    }
    for (const auto& query : asked.textures) {
        const auto& source = query.source;
        if (source.count != 1 || source.shift_left >= 32 || source.secondary_shift_left >= 32 ||
            !readable(source.cbuf_index, source.cbuf_offset) ||
            (source.has_secondary && !readable(source.secondary_cbuf_index, source.secondary_cbuf_offset)))
            return differ("texture handle");
        std::vector<std::pair<u32, u32>> reads{{source.cbuf_index, source.cbuf_offset}};
        if (source.has_secondary) reads.emplace_back(source.secondary_cbuf_index, source.secondary_cbuf_offset);
        for (const auto& [bank, offset] : reads) {
            if (live.HasHLEMacroState() && live.GetReplaceConstBuffer(bank, offset)) return differ("HLE replacement");
            answers.constants.try_emplace((u64{bank} << 32) | offset, live.ReadCbufValue(bank, offset));
        }
        const u32 handle = live.ResolveTextureHandle(source);
        switch (query.kind) {
        case Shader::TextureQueryKind::Type: {
            const auto answer = live.ReadTextureType(handle);
            if (static_cast<u32>(answer) != query.answer) return differ("texture type");
            answers.texture_types.emplace(handle, answer);
            break;
        }
        case Shader::TextureQueryKind::Format: {
            const auto answer = live.ReadTexturePixelFormat(handle);
            // Raw image loads specialize on the exact format (RawImageReadFormat).
            if (static_cast<u32>(answer) != query.answer) return differ("texture format");
            answers.pixel_formats.emplace(handle, answer);
            break;
        }
        case Shader::TextureQueryKind::Integer:
            if (live.IsTexturePixelFormatInteger(handle) != (query.answer != 0)) return differ("texture format");
            answers.pixel_formats.emplace(handle, live.ReadTexturePixelFormat(handle));
            break;
        }
    }
    // Asked by handle, outside a query: a generated handle means nothing to the live draw.
    for (const auto handle : asked.texture_types) {
        const auto found = q.texture_types.find(handle);
        if (found == q.texture_types.end() || live.ReadTextureType(handle) != found->second) return differ("texture type");
        answers.texture_types.emplace(handle, found->second);
    }
    for (const auto handle : asked.pixel_formats) {
        const auto found = q.pixel_formats.find(handle);
        if (found == q.pixel_formats.end() || live.ReadTexturePixelFormat(handle) != found->second) return differ("texture format");
        answers.pixel_formats.emplace(handle, found->second);
    }
    return answers;
} catch (const std::exception&) {
    mismatch = "live read";
    return std::nullopt;
}
} // namespace VideoCommon::ShaderProfile
