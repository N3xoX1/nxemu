// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/shader_profile_journal.h"
#include "yuzu_common/cityhash.h"
#include <atomic>
#include <memory>
#include <bit>
#include <cstring>

namespace Vulkan {
struct GuestInputHash {
    u64 operator()(const void* data, size_t size) const noexcept {
        return Common::CityHash64(static_cast<const char*>(data), size);
    }
};

// Ephemeral, bounded memoization only. Hash matches always compare complete raw
// input words/keys; eviction merely causes another canonical capture. No shader
// instructions, guest addresses, persistent identities or renderer keys change.
template <class Hash = GuestInputHash>
class GuestRecipeInputCache {
    static constexpr size_t Groups=7, Sets=512, Ways=4, Slots=Sets*Ways;
    using Key=std::array<u64,Groups+3>;
    struct RawSlot { u64 hash{}, id{}; };
    struct Group {
        std::vector<u16> methods;
        std::vector<u32> current, values;
        std::vector<RawSlot> slots;
        std::vector<u8> next;
    };
public:
    void EnableDiagnosticKeys() noexcept { diagnostic_keys = true; }
    const Key& DiagnosticKey() const noexcept { return diagnostic_key; }
    struct State {
        Key key{};
        // Non-owning: recorder journal nodes stay stable until recorder teardown.
        const VideoCommon::ShaderProfile::Journal::Entry* entry{};
        FixedPipelineState projected{};
        bool valid{};
    };
    struct Result { State* state; bool hit; };
private:
    struct Storage {
        std::array<Group,Groups+1> groups;
        std::array<State,Slots> states;
        std::array<u8,Sets> next{};
    };
public:
    // Ids of one cache never collide with another's, so a memo left by an
    // earlier recorder can only miss, never match other prefix words.
    GuestRecipeInputCache() : storage(std::make_unique<Storage>()),
        generation(instances.fetch_add(1, std::memory_order_relaxed) << 40) {
        auto dirty=std::make_unique<Tegra::Engines::Maxwell3D::DirtyState>();
        GuestPipelineState::Track(*dirty);
        static_assert(Maxwell::NUM_REGS <= UINT16_MAX);
        for(size_t method=0;method<Maxwell::NUM_REGS;++method)
            for(size_t group=0;group<Groups;++group)
                if(dirty->shader_profile_registers[method] & (1U<<group))
                    storage->groups[group].methods.push_back(static_cast<u16>(method));
        for(size_t i=0;i<=Groups;++i) {
            auto& group=storage->groups[i];
            group.current.resize(i==Groups?104:group.methods.size());
            // Shader combinations need more ways to survive revisits than the
            // low-cardinality register groups. Keep all policies bounded; a
            // collision/eviction still falls back to exact canonical capture.
            const size_t slots=i==Groups?4096:(1U<<i)==GuestPipelineState::VertexInput?1024:256;
            group.slots.resize(slots);
            group.next.resize(slots/Ways);
            group.values.resize(group.current.size()*slots);
        }
    }
    Result Find(const Maxwell& regs, u64 changes, bool full,
                Maxwell::PrimitiveTopology topology,
                Tegra::Engines::Maxwell3D::EngineHint hint, u32 features,
                const void* prefix, bool prefix_changed, u64 known_prefix=0) {
        // A known prefix id was assigned to these exact prefix words earlier;
        // ids are never reused, so it stands for them without hashing again.
        // Input traces keep hashing so their keys stay comparable.
        if(diagnostic_keys)known_prefix=0;
        if(known_prefix)key[Groups+2]=known_prefix;
        u64 remaining=(full?GuestPipelineState::All:changes&GuestPipelineState::All)|
                      (!known_prefix && (full || prefix_changed)?1U<<Groups:0U);
        while(remaining) {
            const size_t i=std::countr_zero(remaining);
            remaining&=remaining-1;
            auto& group=storage->groups[i];
            if(group.current.empty())continue;
            if(i==Groups)std::memcpy(group.current.data(),prefix,104*sizeof(u32));
            else for(size_t word=0;word<group.methods.size();++word)
                group.current[word]=regs.reg_array[group.methods[word]];
            const auto hash=Hash{}(group.current.data(),group.current.size()*sizeof(u32));
            if (diagnostic_keys) diagnostic_key[i==Groups?Groups+2:i]=hash;
            const size_t slots=group.slots.size();
            const size_t set=hash & (group.next.size()-1), begin=set*Ways;
            size_t selected=slots;
            for(size_t way=0;way<Ways;++way) {
                const size_t index=begin+way;
                const auto& slot=group.slots[index];
                if(slot.id && slot.hash==hash && std::equal(group.current.begin(),group.current.end(),
                    group.values.begin()+index*group.current.size())) {
                    selected=index;break;
                }
            }
            if(selected==slots) {
                selected=begin+group.next[set]++%Ways;
                if(++generation==0)throw std::runtime_error("Guest input cache generation exhausted");
                group.slots[selected]={hash,generation};
                std::copy(group.current.begin(),group.current.end(),group.values.begin()+selected*group.current.size());
            }
            key[i==Groups?Groups+2:i]=group.slots[selected].id;
        }
        key[Groups]=static_cast<u32>(topology)|(u64{static_cast<u32>(hint)}<<32);
        key[Groups+1]=features;
        if (diagnostic_keys) { diagnostic_key[Groups]=key[Groups]; diagnostic_key[Groups+1]=features; }
        const size_t set=Hash{}(key.data(),sizeof(key))%Sets, begin=set*Ways;
        for(size_t way=0;way<Ways;++way) {
            auto& state=storage->states[begin+way];
            if(state.valid && state.entry && state.key==key)return {&state,true};
        }
        auto& state=storage->states[begin+storage->next[set]++%Ways];
        // Preserve projected until the caller supplies the replacement. This
        // permits reusing the previous projection for a prefix-only miss, even
        // when the replacement happens to evict that same State slot.
        state.valid=true;state.key=key;state.entry=nullptr;
        return {&state,false};
    }
    // Id of the prefix words of the last Find; never zero after the first one.
    u64 PrefixId() const noexcept { return key[Groups+2]; }
    size_t StorageBytes() const noexcept {
        size_t bytes=sizeof(Storage);
        for(const auto& group:storage->groups)
            bytes+=group.methods.capacity()*sizeof(u16)+(group.current.capacity()+group.values.capacity())*sizeof(u32)+
                   group.slots.capacity()*sizeof(RawSlot)+group.next.capacity()*sizeof(u8);
        return bytes;
    }
private:
    std::unique_ptr<Storage> storage;
    Key key{};
    Key diagnostic_key{};
    bool diagnostic_keys{};
    u64 generation{};
    static inline std::atomic<u64> instances{};
};
} // namespace Vulkan
