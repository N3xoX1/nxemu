// Additional scaling measurements, using the production bodies extracted by pr396.
#define main PriorCpuMain
#include "../pr396/synthetic.cpp"
#undef main
#include "yuzu_video_core/buffer_cache/buffer_base.h"

void SparseReferenceRegistration() {
    VideoCommon::BufferBase real_buffer{VideoCommon::NullBufferParams{}};
    Check("new_production_buffer_has_no_sparse_references", !real_buffer.has_sparse_references);
    Harness h;
    const auto canonical = h.AddBuffer(4096, 4096);
    auto& buffer = h.slot_buffers[canonical];
    buffer.has_sparse_references = false;
    Harness::SparseBufferPart part{4096, 1024, 0, 0, 1};
    h.ResolveSparseBufferPart(part);
    Check("resolved_part_uses_canonical_buffer", part.buffer_id == canonical);
    Check("resolved_part_registers_canonical_buffer", buffer.has_sparse_references);
    const auto replacement = h.AddBuffer(12288, 4096);
    h.slot_buffers[replacement].has_sparse_references = false;
    part.buffer_id = 0;
    part.device_addr = 12288;
    h.ResolveSparseBufferPart(part);
    Check("invalidated_part_selects_replacement", part.buffer_id == replacement);
    Check("invalidated_part_registers_replacement", h.slot_buffers[replacement].has_sparse_references);
    part = {4096, 1024, 0, canonical, 1};
    buffer.has_sparse_references = false;
    h.ResolveSparseBufferPart(part);
    Check("retained_part_conservatively_registers_buffer", buffer.has_sparse_references);
    h.sparse_buffers.push_back({h.gpu_memory, 0x10000, 2048, 2048, canonical, 1, 0, 1,
        {{4096, 1024, 0, canonical, 1}, {5120, 1024, 1024, canonical, 1}}});
    h.MarkBufferContentChanged(buffer, 4096, 16);
    Check("referenced_overlap_remains_stale", h.sparse_buffers[0].parts[0].content_version == 1);
    Check("referenced_disjoint_part_is_current", h.sparse_buffers[0].parts[1].content_version == buffer.content_version);
    h.MarkBufferContentChanged(buffer, 4096, 16);
    Check("already_stale_overlap_stays_stale", h.sparse_buffers[0].parts[0].content_version == 1);
    Check("disjoint_part_tracks_repeated_writes", h.sparse_buffers[0].parts[1].content_version == buffer.content_version);
    h.sparse_buffers.clear(); // The conservative flag intentionally survives part removal.
    const auto previous = buffer.content_version;
    h.MarkBufferContentChanged(buffer, 4096, 16);
    Check("removed_parts_allow_future_version_updates", buffer.content_version > previous);
    const auto unrelated = h.AddBuffer(8192, 4096);
    h.slot_buffers[unrelated].has_sparse_references = false;
    const auto old_unrelated = h.slot_buffers[unrelated].content_version;
    h.MarkBufferContentChanged(h.slot_buffers[unrelated], 8192, 16);
    Check("unreferenced_buffer_still_advances_version", h.slot_buffers[unrelated].content_version > old_unrelated);
    real_buffer.has_sparse_references = true;
    VideoCommon::BufferBase moved_buffer{std::move(real_buffer)};
    Check("moved_production_buffer_keeps_sparse_registration", moved_buffer.has_sparse_references);
}

void SparseReferenceOracle() {
    Harness h;
    std::array<BufferId, 8> buffers;
    for (u32 i = 0; i < buffers.size(); ++i) {
        buffers[i] = h.AddBuffer(4096ULL * (i + 1), 4096);
        h.slot_buffers[buffers[i]].has_sparse_references = false;
    }
    for (u32 i = 0; i < 4; ++i) {
        Harness::SparseBuffer sparse{h.gpu_memory, 0x10000ULL + i * 0x10000ULL,
                                    4096, 4096, buffers[i], 1, 0, 1, {}};
        for (u32 j = 0; j < 4; ++j) {
            auto& part = sparse.parts.emplace_back(Harness::SparseBufferPart{
                4096ULL * (i + 1) + j * 1024ULL, 1024, j * 1024, buffers[i], 1});
            h.ResolveSparseBufferPart(part);
        }
        h.sparse_buffers.push_back(std::move(sparse));
    }
    std::mt19937 rng{0x1072026};
    bool valid = true;
    for (u32 iteration = 0; iteration < 2000; ++iteration) {
        const auto target = buffers[rng() % buffers.size()];
        auto& buffer = h.slot_buffers[target];
        const auto previous = buffer.content_version;
        const auto next = h.content_version + 1;
        const DAddr begin = buffer.base + rng() % 4096;
        const u64 size = iteration % 63 == 0 ? std::numeric_limits<u64>::max() : rng() % 513;
        std::vector<u64> expected;
        for (const auto& sparse : h.sparse_buffers) for (const auto& part : sparse.parts) {
            const bool update = size != std::numeric_limits<u64>::max() && part.buffer_id == target &&
                part.content_version == previous && (part.device_addr >= begin + size || part.device_addr + part.size <= begin);
            expected.push_back(update ? next : part.content_version);
        }
        h.MarkBufferContentChanged(buffer, begin, size);
        valid &= buffer.content_version == next;
        size_t index = 0;
        for (const auto& sparse : h.sparse_buffers) for (const auto& part : sparse.parts)
            valid &= part.content_version == expected[index++];
    }
    Check("sparse_versions_match_original_semantics_2000_writes", valid);
}

void VersionScaling() {
    for (u32 count : {0U, 1U, 8U, 64U, 512U, 4096U}) {
        Harness h;
        const auto target = h.AddBuffer(4096, 4096);
        h.slot_buffers[target].has_sparse_references = false;
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
    SparseReferenceRegistration();
    SparseReferenceOracle();
    VersionScaling();
    FragmentedCopyScaling();
    MaskScanning();
    std::cout << "RESULT tests=" << tests << " failed=" << failures << '\n';
    return failures ? 1 : 0;
}
