// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>

#include <boost/container/small_vector.hpp>

namespace VideoCommon {

// Records descriptors only when first resolved. Entries own their keys, so a channel/map
// rehash or erase can never leave dangling pointers in the reverse index.
template <class ViewId, class Descriptor>
class ImageViewDescriptorIndex {
public:
    static constexpr size_t MaxReferencesPerView = 64;

    void Register(ViewId view, size_t channel, const Descriptor& descriptor) {
        Entry& entry = refs[view];
        if (entry.dense) {
            return;
        }
        if (entry.references.size() == MaxReferencesPerView) {
            // Very popular views are cheaper to remove with the original batched scan.
            // Stop accumulating duplicate keys and avoid an unbounded secondary copy.
            entry.references.clear();
            entry.references.shrink_to_fit();
            entry.dense = true;
            return;
        }
        entry.references.emplace_back(channel, descriptor);
    }

    [[nodiscard]] bool HasDenseViews(std::span<const ViewId> views) const {
        for (const ViewId view : views) {
            if (const auto it = refs.find(view); it != refs.end() && it->second.dense) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] size_t CountReferences(std::span<const ViewId> views) const {
        size_t count = 0;
        for (const ViewId view : views) {
            if (const auto it = refs.find(view); it != refs.end()) {
                count += it->second.references.size();
            }
        }
        return count;
    }

    void ForgetViews(std::span<const ViewId> views) {
        for (const ViewId view : views) {
            refs.erase(view);
        }
    }

    template <class RemoveEntry>
    void Invalidate(std::span<const ViewId> removed, RemoveEntry&& remove_entry) {
        for (const ViewId view : removed) {
            const auto it = refs.find(view);
            if (it == refs.end()) {
                continue;
            }
            for (const auto& [channel, descriptor] : it->second.references) {
                remove_entry(channel, descriptor, view);
            }
            refs.erase(it);
        }
    }

    template <class ChannelEntries>
    void ForgetChannel(size_t channel, const ChannelEntries& entries) {
        for (const auto& [descriptor, view] : entries) {
            const auto it = refs.find(view);
            if (it == refs.end()) {
                continue;
            }
            if (it->second.dense) {
                continue;
            }
            auto& values = it->second.references;
            values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
                             return value.first == channel && value.second == descriptor;
                         }), values.end());
            if (values.empty()) {
                refs.erase(it);
            }
        }
    }

private:
    struct Entry {
        boost::container::small_vector<std::pair<size_t, Descriptor>, 2> references;
        bool dense{};
    };
    std::unordered_map<ViewId, Entry> refs;
};

} // namespace VideoCommon
