// Additional scaling measurements, using the production bodies extracted by pr396.
#define main PriorCpuMain
#include "../pr396/synthetic.cpp"
#undef main

void VersionScaling() {
    for (u32 count : {0U, 1U, 8U, 64U, 512U, 4096U}) {
        Harness h;
        const auto target = h.AddBuffer(4096, 4096);
        const auto other = h.AddBuffer(8192, 4096);
        for (u32 i = 0; i < count; ++i) {
            h.sparse_buffers.push_back({h.gpu_memory, 0x10000ULL + i * 0x10000ULL,
                4096, 4096, other, 1, 0, 1,
                {{8192, 1024, 0, other, 1}, {9216, 1024, 1024, other, 1},
                 {10240, 1024, 2048, other, 1}, {11264, 1024, 3072, other, 1}}});
        }
        const auto name = "version_update_unrelated_sparse_buffers_" + std::to_string(count);
        Bench(name.c_str(), count < 512 ? 100000 : 3000, [&](u64 i) {
            h.MarkBufferContentChanged(h.slot_buffers[target], 4096 + (i & 63), 4);
            sink = h.slot_buffers[target].content_version;
        });
        Check("unrelated_sparse_versions_preserved",
              std::ranges::all_of(h.sparse_buffers, [](const auto& s) {
                  return std::ranges::all_of(s.parts, [](const auto& p) {
                      return p.content_version == 1;
                  });
              }));
    }
}

void FragmentedCopyScaling() {
    for (u32 parts : {1U, 8U, 64U, 256U}) {
        Harness h;
        const auto assembled = h.AddBuffer(0, parts * 64, 0x22);
        Harness::SparseBuffer sparse{h.gpu_memory, 0x10000, parts * 64, parts * 64,
                                    assembled, 1, 0, 1, {}};
        for (u32 i = 0; i < parts; ++i) {
            const auto canonical = h.AddBuffer(4096 + i * 4096ULL, 64);
            sparse.parts.push_back({4096 + i * 4096ULL, 64, i * 64, canonical, 1});
        }
        h.sparse_buffers.push_back(std::move(sparse));
        h.runtime.execute = false; // Measure cache bookkeeping, excluding mock byte copies.
        const auto name = "sparse_copyback_parts_" + std::to_string(parts);
        Bench(name.c_str(), parts < 64 ? 10000 : 1000, [&](u64) {
            h.MarkSparseWrite(0x10000, parts * 64);
            h.CopySparseWrites();
            sink = h.runtime.copy_calls;
        });
        const u64 iterations = parts < 64 ? 10000 : 1000;
        Check("one_copy_per_resident_part", h.runtime.copy_calls == 7 * iterations * parts);
        Check("copyback_exact_total_bytes", h.runtime.copy_bytes == 7 * iterations * parts * 64);
    }
}

void MaskScanning() {
    // This is a model of FinishRuntimeImageWrites' CPU mask loop, not a shader benchmark.
    for (u32 words : {1U, 33U, 257U}) {
        std::vector<u32> mask(words, 0);
        mask[words / 2] = 1U << 3;
        const auto name = "runtime_write_mask_cpu_scan_words_" + std::to_string(words);
        Bench(name.c_str(), 100000, [&](u64) {
            u32 found = 0;
            for (u32 bits : mask) for (u32 bit = 0; bit < 32; ++bit)
                found += (bits & (1U << bit)) != 0;
            sink = found;
        });
        Check("single_written_handle", sink == 1);
    }
}

int main() {
    VersionScaling();
    FragmentedCopyScaling();
    MaskScanning();
    std::cout << "RESULT tests=" << tests << " failed=" << failures << '\n';
    return failures ? 1 : 0;
}
