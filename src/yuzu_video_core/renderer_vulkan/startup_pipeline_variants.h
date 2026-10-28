// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vulkan
{
// Startup only. Retain the first value solely while all observed inputs agree.
// Conflicts invalidate the whole key, independent of record order. Disk records
// remain distinct; normal gameplay can compile the actual environment later.
template <class Key, class Inputs>
class StartupPipelineVariants
{
public:
    StartupPipelineVariants()=default;
    // The order vector points into this map. A generated copy would retain
    // pointers to the original collector. This startup-local helper is immobile.
    StartupPipelineVariants(const StartupPipelineVariants&)=delete;
    StartupPipelineVariants& operator=(const StartupPipelineVariants&)=delete;
    StartupPipelineVariants(StartupPipelineVariants&&)=delete;
    StartupPipelineVariants& operator=(StartupPipelineVariants&&)=delete;
    struct Candidate
    {
        Inputs inputs;
        bool ambiguous{};
    };
    template <class Equal>
    void Observe(const Key & key, Inputs inputs, Equal equal)
    {
        const auto it = entries.find(key);
        if (it == entries.end())
        {
            const auto saved = entries.emplace(key, Candidate{std::move(inputs), false}).first;
            // unordered_map may rehash, but references to node keys remain valid.
            try {order.push_back(&*saved);}
            catch(...) {entries.erase(saved);throw;}
        }
        else if (!it->second.ambiguous && !equal(it->second.inputs, inputs))
            it->second.ambiguous = true;
    }
    template <class F, class Rejected>
    size_t ConsumeSafe(F consume, Rejected reject)
    {
        // Detach the whole batch without copying keys. Callback exceptions
        // discard the interrupted batch; a later call never retries moved inputs.
        // Observations made by callbacks belong to the next batch.
        Map batch;
        batch.swap(entries);
        std::vector<typename Map::value_type*> pending;
        pending.swap(order);
        size_t rejected{};
        for (auto * entry : pending)
        {
            const auto & key = entry->first;
            auto & candidate = entry->second;
            if (candidate.ambiguous)
            {
                ++rejected;
                reject(key);
            }
            else
                consume(key, std::move(candidate.inputs));
        }
        return rejected;
    }

    template <class F>
    size_t ConsumeSafe(F consume)
    {
        return ConsumeSafe(std::move(consume), [](const Key &) {});
    }

private:
    using Map = std::unordered_map<Key, Candidate>;
    Map entries;
    // Keep stable pointers to hash-table nodes: no key copies, no extra
    // hash lookups and no dependence on unordered_map iteration order.
    std::vector<typename Map::value_type*> order;
};
} // namespace Vulkan
