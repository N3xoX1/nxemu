// Microbenchmark of descriptor lookup, not a prediction of whole-game FPS.
// Uses the production DescriptorTable with a deterministic in-memory guest.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include "yuzu_video_core/texture_cache/descriptor_table.h"

using Descriptor = std::array<u32, 8>;
struct Result { double milliseconds; u64 checksum; unsigned reads; };

// Frozen scalar algorithm from master, so changes to the optimized Read() cannot
// accidentally make the benchmark's reference slower as well.
struct ScalarReference {
    Tegra::MemoryManager& memory;
    std::vector<Descriptor> descriptors;
    std::vector<u64> read;
    Descriptor Read(u32 index) {
        Descriptor value;
        memory.ReadBlockUnsafe(256 + size_t{index} * sizeof(Descriptor), &value, sizeof(value));
        const u64 bit = u64{1} << (index % 64);
        const bool changed = !(read[index / 64] & bit) || value != descriptors[index];
        read[index / 64] |= bit;
        if (changed) descriptors[index] = value;
        return value;
    }
};

Result Run(u32 count, u32 stride, bool batch, unsigned mode) {
    Tegra::MemoryManager memory;
    memory.contiguous = mode == 1;
    memory.fragmented = mode == 2;
    memory.bytes.resize(256 + size_t{count} * sizeof(Descriptor));
    for (u32 i = 0; i < count; ++i) {
        Descriptor descriptor{};
        if (stride && i % stride == 0) { descriptor[0] = i + 1; descriptor[7] = i ^ 0x12345678; }
        std::memcpy(memory.bytes.data() + 256 + size_t{i} * sizeof(Descriptor),
                    &descriptor, sizeof(descriptor));
    }
    VideoCommon::DescriptorTable<Descriptor> table{memory};
    (void)table.Synchronize(256, count - 1);
    ScalarReference reference{memory, std::vector<Descriptor>(count), std::vector<u64>((count + 63) / 64)};
    u64 checksum{};
    constexpr u32 rounds = 20;
    const auto start = std::chrono::steady_clock::now();
    for (u32 round = 0; round < rounds; ++round) {
        // A newly changed descriptor must be visible on every pass.
        const u32 index = (round * 7919U) % count;
        Descriptor changed{}; changed[0] = index + 1; changed[7] = round + 17;
        std::memcpy(memory.bytes.data() + 256 + size_t{index} * sizeof(Descriptor),
                    &changed, sizeof(changed));
        const bool active_batch = batch && table.BeginReadBatch();
        for (u32 i = 0; i < count;) {
            if (active_batch && table.BatchBlockEmpty(i)) {
                i += static_cast<u32>(table.BATCH_BLOCK_ENTRIES);
                continue;
            }
            const u32 current = i++;
            if (active_batch && (*table.BatchedDescriptor(current))[0] == 0) continue;
            const auto descriptor = batch ? table.Read(current).first : reference.Read(current);
            checksum += descriptor[0] + u64{descriptor[7]};
        }
        if (active_batch) table.EndReadBatch();
    }
    return {std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / rounds,
            checksum, memory.reads};
}

int main() {
    std::puts("entries,stride,span_mode,scalar_ms,batch_ms,speedup,scalar_reads,batch_reads,checksum");
    for (u32 count : {128U, 4096U, 524288U}) {
        for (u32 stride : {0U, 1024U, 1U}) {
          for (unsigned mode : {0U, 1U, 2U}) {
            std::array<double, 5> scalar{}, batch{};
            Result a{}, b{};
            for (size_t repeat = 0; repeat < scalar.size(); ++repeat) {
                // Alternate order so CPU frequency/cache warmup does not favor one mode.
                if (repeat % 2) { b = Run(count, stride, true, mode); a = Run(count, stride, false, mode); }
                else { a = Run(count, stride, false, mode); b = Run(count, stride, true, mode); }
                if (a.checksum != b.checksum) return 1;
                scalar[repeat] = a.milliseconds; batch[repeat] = b.milliseconds;
            }
            std::sort(scalar.begin(), scalar.end()); std::sort(batch.begin(), batch.end());
            std::printf("%u,%u,%u,%.6f,%.6f,%.3f,%u,%u,%llu\n", count, stride, mode,
                        scalar[2], batch[2], scalar[2] / batch[2], a.reads, b.reads, a.checksum);
          }
        }
    }
}
