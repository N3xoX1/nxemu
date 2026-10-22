// SPDX-License-Identifier: GPL-3.0-or-later
#include "query_bank_operations.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

struct Bank {
    size_t size = 256, next_bank = 0, id = 0;
    size_t Size() const { return size; }
};
struct Pool {
    std::array<Bank, 3> banks{{{256, 2, 0}, {255, 3, 1}, {256, 0, 2}}};
    Bank& GetBank(size_t id) {
        if (id >= banks.size())
            throw std::runtime_error("Traversal used a recycled link");
        return banks[id];
    }
};
struct Query {
    size_t start_bank_id = 0, size_banks = 3, start_slot = 254, size_slots = 258;
};
template <typename Func> void Previous(const Query& query, Pool& pool, Func&& func) {
    size_t remaining = query.size_slots;
    if (!remaining)
        return;
    size_t bank = query.start_bank_id, start = query.start_slot;
    for (size_t i = 0; i < query.size_banks; ++i) {
        auto& current = pool.GetBank(bank);
        const size_t amount = std::min(current.Size() - start, remaining);
        func(&current, start, amount);
        bank = current.next_bank - 1;
        start = 0;
        remaining -= amount;
    }
}
void Require(bool result, const char* message) {
    if (!result)
        throw std::runtime_error(message);
}

static std::atomic<unsigned long long> result_sink{};
template <bool Fixed, bool ReleaseReferences = false> double Benchmark(size_t count) {
    Pool pool;
    Query query;
    unsigned long long checksum = 0;
    std::atomic<size_t> references[3];
    for (auto& refs : references)
        refs.store(size_t{1} << 62, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i) {
        // Vary a real input so the compiler cannot reduce this to one multiply.
        query.start_slot = 253 + (i & 1);
        auto consume = [&](Bank* bank, size_t offset, size_t amount) {
            checksum += bank->id + offset + amount;
            if constexpr (ReleaseReferences) {
                references[bank->id].fetch_sub(amount, Fixed ? std::memory_order_release
                                                             : std::memory_order_relaxed);
            }
        };
        if constexpr (Fixed)
            VideoCommon::ApplyQueryBankOp(query, pool, consume);
        else
            Previous(query, pool, consume);
    }
    const auto end = std::chrono::steady_clock::now();
    result_sink.fetch_add(checksum, std::memory_order_relaxed);
    return std::chrono::duration<double, std::nano>(end - start).count() / count;
}

int main() try {
    Query query;
    Pool pool;
    bool reproduced = false;
    try {
        Previous(query, pool, [](Bank* bank, size_t, size_t) {
            bank->next_bank = 0; // Reset/recycle as soon as the last reference drops.
        });
    } catch (const std::runtime_error&) {
        reproduced = true;
    }
    Require(reproduced, "Old traversal did not reproduce recycled-link failure");

    pool = Pool{};
    std::vector<size_t> visited, amounts;
    VideoCommon::ApplyQueryBankOp(query, pool, [&](Bank* bank, size_t, size_t amount) {
        visited.push_back(bank->id);
        amounts.push_back(amount);
        bank->next_bank = 0;
    });
    Require(visited == std::vector<size_t>{0, 1, 2}, "Wrong bank after recycling");
    Require(amounts == std::vector<size_t>{2, 255, 1}, "Partial-bank ranges changed");

    pool = Pool{};
    std::atomic<unsigned> phase{};
    std::thread recycler([&] {
        phase.wait(0, std::memory_order_acquire);
        pool.banks[0].next_bank = 0;
        phase.store(2, std::memory_order_release);
        phase.notify_one();
    });
    visited.clear();
    VideoCommon::ApplyQueryBankOp(query, pool, [&](Bank* bank, size_t, size_t) {
        visited.push_back(bank->id);
        if (bank->id == 0) {
            phase.store(1, std::memory_order_release);
            phase.notify_one();
            while (phase.load(std::memory_order_acquire) != 2)
                phase.wait(1, std::memory_order_acquire);
        }
    });
    recycler.join();
    Require(visited == std::vector<size_t>{0, 1, 2}, "Concurrent recycler changed traversal");

    query.size_slots = 0;
    VideoCommon::ApplyQueryBankOp(query, pool, [](Bank*, size_t, size_t) {
        throw std::runtime_error("Empty query touched a bank");
    });
    std::puts("Old recycled-link failure reproduced: PASS\nPartial and recycled banks: "
              "PASS\nConcurrent recycler: PASS\nEmpty counter: PASS");

    std::vector<double> previous, fixed;
    for (unsigned i = 0; i < 9; ++i) {
        if (i & 1) {
            fixed.push_back(Benchmark<true>(20000000));
            previous.push_back(Benchmark<false>(20000000));
        } else {
            previous.push_back(Benchmark<false>(20000000));
            fixed.push_back(Benchmark<true>(20000000));
        }
    }
    std::sort(previous.begin(), previous.end());
    std::sort(fixed.begin(), fixed.end());
    std::printf("CPU traversal median ns/counter: previous %.3f, fixed %.3f, ratio %.4f\n",
                previous[4], fixed[4], fixed[4] / previous[4]);
    previous.clear();
    fixed.clear();
    for (unsigned i = 0; i < 9; ++i) {
        if (i & 1) {
            fixed.push_back(Benchmark<true, true>(2000000));
            previous.push_back(Benchmark<false, true>(2000000));
        } else {
            previous.push_back(Benchmark<false, true>(2000000));
            fixed.push_back(Benchmark<true, true>(2000000));
        }
    }
    std::sort(previous.begin(), previous.end());
    std::sort(fixed.begin(), fixed.end());
    std::printf(
        "CPU retirement median ns/counter (3 banks): previous %.3f, fixed %.3f, ratio %.4f\n",
        previous[4], fixed[4], fixed[4] / previous[4]);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
