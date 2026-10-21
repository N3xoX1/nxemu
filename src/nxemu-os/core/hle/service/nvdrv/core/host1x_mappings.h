// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Service::Nvidia::NvCore
{
class PinnedHandle;

// NVDEC and VIC currently share one Host1x address space. A submitted job must
// retain mappings made by either device, including buffers shared across FDs.
class Host1xMappings
{
public:
    using Set = std::vector<std::shared_ptr<PinnedHandle>>;

    void Replace(const void * owner, std::shared_ptr<const Set> next)
    {
        std::shared_ptr<const Set> previous;
        std::shared_ptr<const Set> previous_snapshot;
        {
            std::scoped_lock lock{mutex};
            const auto it = owners.find(owner);
            if (!next || next->empty())
            {
                if (it != owners.end())
                {
                    previous = std::move(it->second);
                    owners.erase(it);
                }
            }
            else if (it != owners.end())
            {
                previous = std::exchange(it->second, std::move(next));
            }
            else
            {
                owners.emplace(owner, std::move(next));
            }
            previous_snapshot = std::move(snapshot);
        }
        // The last reference may unmap device memory. Do so outside this mutex.
    }

    std::shared_ptr<const Set> Snapshot()
    {
        std::scoped_lock lock{mutex};
        if (!snapshot)
        {
            auto next = std::make_shared<Set>();
            for (const auto & [owner, mappings] : owners)
            {
                next->insert(next->end(), mappings->begin(), mappings->end());
            }
            snapshot = std::move(next);
        }
        return snapshot;
    }

private:
    std::mutex mutex;
    std::unordered_map<const void *, std::shared_ptr<const Set>> owners;
    std::shared_ptr<const Set> snapshot;
};
} // namespace Service::Nvidia::NvCore
