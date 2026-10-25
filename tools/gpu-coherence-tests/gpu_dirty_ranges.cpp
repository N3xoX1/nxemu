// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_common/common_types.h"
#include "nxemu-module-spec/operating_system.h"
#include "nxemu-os/core/gpu_dirty_memory_manager.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <set>
#include <thread>
#include <atomic>

struct Invalidator : ICacheInvalidator {
    std::set<u64> blocks;
    void OnCacheInvalidation(uint64_t address, uint32_t size) override {
        for (u64 offset = 0; offset < size; offset += 64) blocks.insert((address + offset) / 64);
    }
};
void Verify(u64 address, size_t size) {
    Core::GPUDirtyMemoryManager manager;
    Invalidator invalidator;
    manager.Collect(address, size);
    manager.Gather(&invalidator);
    std::set<u64> expected;
    for (u64 block = address / 64; size && block <= (address + size - 1) / 64; ++block) {
        expected.insert(block);
        if (!invalidator.blocks.contains(block)) {
            std::cerr << "Lost CPU-write invalidation: address=0x" << std::hex << address
                      << " size=0x" << size << " missing block=0x" << block * 64 << '\n';
            throw std::runtime_error("GPU dirty range lost");
        }
    }
    if (invalidator.blocks != expected) throw std::runtime_error("Unrelated CPU-write blocks invalidated");
    const auto before = invalidator.blocks;
    manager.Gather(&invalidator);
    if (before != invalidator.blocks) throw std::runtime_error("Empty gather added dirty bytes");
}
int main() {
    try {
        Verify(0x56260000, 4096); // Real WriteBlock/ZeroBlock notifications can span a 4-KiB page.
        for (u64 offset : {0, 1, 63, 64, 1984, 2016, 2032, 2047, 2048, 4064, 4095})
            for (size_t size : {0, 1, 8, 16, 32, 64, 128, 2048, 4096, 8192}) Verify(0x56260000 + offset, size);
        Core::GPUDirtyMemoryManager manager;
        Invalidator invalidator;
        for (u64 i = 0; i < 100; ++i) manager.Collect(0x56260000 + i * 4096 + 2032, 64);
        manager.Gather(&invalidator);
        for (u64 i = 0; i < 100; ++i) for (u64 offset : {1984, 2048})
            if (!invalidator.blocks.contains((0x56260000 + i * 4096 + offset) / 64))
                throw std::runtime_error("Accumulated split range lost");
        Core::GPUDirtyMemoryManager concurrent;
        Invalidator gathered;
        std::atomic<bool> finished = false;
        std::thread consumer([&] {
            while (!finished.load(std::memory_order_acquire)) concurrent.Gather(&gathered);
        });
        for (u64 i = 0; i < 10000; ++i) concurrent.Collect(0x56260000 + i * 4096 + 2032, 64);
        finished.store(true, std::memory_order_release);
        consumer.join();
        concurrent.Gather(&gathered);
        for (u64 i = 0; i < 10000; ++i) for (u64 offset : {1984, 2048})
            if (!gathered.blocks.contains((0x56260000 + i * 4096 + offset) / 64))
                throw std::runtime_error("Concurrent CPU dirty range lost");
        std::cout << "PASS: cross-window, unaligned, full-page, zero-size and accumulated CPU dirty ranges\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
