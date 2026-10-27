// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#include <nxemu-module-spec/cpu.h>

namespace Common {

// Initial modules keep immutable tables. Runtime snapshots are read from signal handlers:
// no lock, allocation, shared_ptr operation or reclamation is permitted on that path.
class NativeCodeMetadata {
    using Active = std::atomic<bool>;
    struct Module {
        uint64_t id;
        std::shared_ptr<Active> active;
        std::vector<NativeCodeEntry> handlers, restarts, atomics;
    };
    struct Entry {
        uint64_t pc, target;
        const Active* active;
    };
    struct Snapshot {
        // Only the writer copies/destroys these owners. Readers access raw flag pointers.
        std::vector<std::shared_ptr<Active>> owners;
        std::vector<Entry> handlers, restarts, atomics;
    };
    struct ReadGuard {
        explicit ReadGuard(const NativeCodeMetadata& table) : table(table) {
            table.readers.fetch_add(1);
            snapshot = table.current.load();
        }
        ~ReadGuard() { table.readers.fetch_sub(1); }
        const NativeCodeMetadata& table;
        const Snapshot* snapshot{};
    };
public:
    ~NativeCodeMetadata() { delete current.load(); }
    uint64_t FindHandler(uint64_t pc) const { return Find(pc, 0); }
    uint64_t FindRestart(uint64_t pc) const { return Find(pc, 1); }
    uint64_t FindAtomic(uint64_t pc) const { return Find(pc, 2); }

    void Install(uint64_t id, std::span<const NativeCodeEntry> handlers,
                 std::span<const NativeCodeEntry> restarts,
                 std::span<const NativeCodeEntry> atomics = {}) {
        std::lock_guard guard(writer);
        modules.push_back({id, std::make_shared<Active>(true),
            {handlers.begin(), handlers.end()}, {restarts.begin(), restarts.end()},
            {atomics.begin(), atomics.end()}});
        try {
            Publish();
        } catch (...) {
            modules.pop_back();
            throw;
        }
    }
    template <typename Callback>
    void VisitAtomics(Callback&& callback) const {
        ReadGuard guard(*this);
        if (guard.snapshot) for (const auto& entry : guard.snapshot->atomics)
            if (entry.active->load()) callback(entry.pc, entry.target);
    }
    void Remove(uint64_t id) {
        std::lock_guard guard(writer);
        for (auto& module : modules) if (module.id == id) module.active->store(false);
        std::erase_if(modules, [id](const auto& module) { return module.id == id; });
        // Unload must not allocate or throw. Inactive entries remain in the current snapshot
        // until the next install; snapshot ownership keeps their flags alive. They never
        // accumulate across reloads, and the last unload releases the snapshot immediately.
        const auto* previous = modules.empty() ? current.exchange(nullptr) : nullptr;
        Quiesce();
        delete previous;
    }
private:
    uint64_t Find(uint64_t pc, unsigned kind) const {
        if (current.load(std::memory_order_relaxed) == nullptr) return 0;
        ReadGuard guard(*this);
        if (!guard.snapshot) return 0;
        const auto& entries = kind == 2 ? guard.snapshot->atomics :
                              kind == 1 ? guard.snapshot->restarts : guard.snapshot->handlers;
        const auto it = std::lower_bound(entries.begin(), entries.end(), pc,
            [](const auto& entry, uint64_t value) { return entry.pc < value; });
        return it != entries.end() && it->pc == pc && it->active->load() ? it->target : 0;
    }
    void Quiesce() const {
        while (readers.load() != 0) std::this_thread::yield();
    }
    void Publish() {
        auto next = std::make_unique<Snapshot>();
        const auto append = [](auto& destination, const auto& source, const Active* active) {
            for (const auto& entry : source) destination.push_back({entry.pc, entry.target, active});
        };
        for (const auto& module : modules) {
            next->owners.push_back(module.active);
            append(next->handlers, module.handlers, module.active.get());
            append(next->restarts, module.restarts, module.active.get());
            append(next->atomics, module.atomics, module.active.get());
        }
        const auto order = [](const auto& a, const auto& b) { return a.pc < b.pc; };
        std::sort(next->handlers.begin(), next->handlers.end(), order);
        std::sort(next->restarts.begin(), next->restarts.end(), order);
        std::sort(next->atomics.begin(), next->atomics.end(), order);
        const auto* previous = current.exchange(next.release());
        // Seq_cst entry/publication/count operations guarantee that readers either hold a
        // count before the exchange or reload the new snapshot after entering. The same
        // ordering ensures new readers see a removed module's inactive flag before access.
        Quiesce();
        delete previous;
    }
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<const Snapshot*>::is_always_lock_free);
    static_assert(Active::is_always_lock_free);
    mutable std::atomic<uint32_t> readers{};
    std::atomic<const Snapshot*> current{};
    std::mutex writer;
    std::vector<Module> modules;
};
} // namespace Common
