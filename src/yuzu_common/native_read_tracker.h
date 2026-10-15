// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Core {

// GPU-cached pages materialized for native CPU reads. Index physical backing, not
// device addresses: an alias created after Begin must invalidate the same lease.
// Lock order: tracker -> process tracking -> native address space. Callers must
// release process tracking locks before entering this tracker or waiting for GPU work.
template <typename Memory>
class NativeReadTracker {
    struct Entry {
        Memory* memory;
        uint64_t address;
        const uint8_t* backing;
        bool active = true;
        bool readable = false;
    };

public:
    using Lease = std::shared_ptr<Entry>;

    bool Active() const { return active.load(std::memory_order_acquire); }

    Lease Begin(Memory* memory, uint64_t address, const uint8_t* backing) {
        std::scoped_lock lock{mutex};
        auto& page = pages[reinterpret_cast<uintptr_t>(backing)];
        // A newly trapped read can follow cache reprotection outside this tracker.
        // Replace its previous lease; never infer permissions from an old token.
        for (auto it = page.begin(); it != page.end();) {
            if ((*it)->memory == memory && (*it)->address == address) {
                (*it)->active = false;
                it = page.erase(it);
            } else {
                ++it;
            }
        }
        auto lease = std::make_shared<Entry>(Entry{memory, address, backing});
        page.push_back(lease);
        active.store(true, std::memory_order_release);
        return lease;
    }

    void Complete(const Lease& lease) {
        std::scoped_lock lock{mutex};
        if (!lease->active) return;
        // A GPU write/unregister can cancel pending work. Completion must not
        // reopen a page whose backing changed or whose process was destroyed.
        lease->readable = lease->memory->SetNativeMemoryReadProtection(
            lease->address, lease->backing, true);
    }

    void Reprotect(uintptr_t backing, size_t size) {
        if (!Active() || size == 0) return;
        std::scoped_lock lock{mutex};
        auto it = pages.lower_bound(backing);
        while (it != pages.end() && it->first - backing < size) {
            for (const auto& lease : it->second) {
                if (lease->readable) {
                    lease->memory->SetNativeMemoryReadProtection(
                        lease->address, lease->backing, false);
                }
                lease->active = false;
            }
            it = pages.erase(it);
        }
        active.store(!pages.empty(), std::memory_order_release);
    }

    void Forget(Memory* memory) {
        std::scoped_lock lock{mutex};
        for (auto it = pages.begin(); it != pages.end();) {
            auto& page = it->second;
            for (auto entry = page.begin(); entry != page.end();) {
                if ((*entry)->memory == memory) {
                    (*entry)->active = false;
                    entry = page.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (page.empty()) it = pages.erase(it);
            else ++it;
        }
        active.store(!pages.empty(), std::memory_order_release);
    }

private:
    std::mutex mutex;
    std::map<uintptr_t, std::vector<Lease>> pages;
    std::atomic_bool active{};
};

} // namespace Core
