// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_nx.h"
#include "nxemu-loader/nxformat/nx_disk_source.hpp"
#include <zstd.h>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace VideoCommon::ShaderProfile {
namespace {
// The same frame rules as the loader: one standard frame, no dictionary, a
// window of at most 1 MiB, exactly the block size.
bool Decode(void* opaque, const uint8_t* frame, size_t size, uint8_t* output, size_t capacity, size_t* written,
            size_t* consumed) {
    if (size < 6 || capacity > 1024 * 1024 || std::memcmp(frame, "\x28\xb5\x2f\xfd", 4) ||
        ZSTD_getFrameContentSize(frame, size) != capacity || ZSTD_getDictID_fromFrame(frame, size) ||
        ZSTD_findFrameCompressedSize(frame, size) != size)
        return false;
    auto context = static_cast<ZSTD_DCtx*>(opaque);
    const auto result = ZSTD_decompressDCtx(context, output, capacity, frame, size);
    if (ZSTD_isError(result) || result != capacity) return false;
    *written = result;
    *consumed = size;
    return true;
}
} // namespace

struct NxGame::Impl {
    std::shared_ptr<nx::DiskSource> source;
    ZSTD_DCtx* decoder{};
    std::optional<nx::Archive> archive;
    std::mutex mutex;
    ~Impl() {
        archive.reset();
        if (decoder) ZSTD_freeDCtx(decoder);
    }
    void Contents(uint32_t snapshot, std::set<std::string>& seen, std::vector<Nca>& out) const {
        const auto root = archive->root_directory(snapshot);
        if (!root) return;
        archive->enumerate(*root.value, [&](const nx::DirectoryEntryView& entry) {
            if (entry.type == nx::EntryType::file && entry.name.ends_with(".nca") && seen.emplace(entry.name).second)
                out.push_back({std::string(entry.name), entry.size, entry.file.value});
            return out.size() < 1024;
        });
    }
};

NxGame::NxGame(const std::filesystem::path& path) : impl(std::make_unique<Impl>()) {
    auto source = nx::DiskSource::open(path);
    if (!source) throw std::runtime_error("Cannot open the NX file");
    impl->source = std::move(*source.value);
    impl->decoder = ZSTD_createDCtx();
    if (!impl->decoder || ZSTD_isError(ZSTD_DCtx_setParameter(impl->decoder, ZSTD_d_windowLogMax, 20)))
        throw std::runtime_error("Cannot create the NX decoder");
    nx::OpenOptions options;
    options.zstd = &Decode;
    options.zstd_context = impl->decoder;
    options.max_cached_block_bytes = 8ull * 1024 * 1024;
    auto archive = nx::Archive::open(*impl->source, options);
    if (!archive) throw std::runtime_error("Not a readable NX file: " + archive.error.message);
    impl->archive.emplace(std::move(*archive.value));
    const auto mounted = impl->archive->mount();
    if (!mounted) throw std::runtime_error("Not a readable NX file: " + mounted.error.message);
}

NxGame::~NxGame() = default;

std::vector<NxGame::Nca> NxGame::AllContents() const {
    std::vector<Nca> result;
    std::set<std::string> seen;
    for (uint32_t i = 0; i < impl->archive->snapshots().size(); ++i) impl->Contents(i, seen, result);
    return result;
}

std::vector<NxGame::Nca> NxGame::PlayedContents(uint64_t title_id) const {
    const auto& archive = *impl->archive;
    const auto& programs = archive.programs();
    const auto& snapshots = archive.snapshots();
    const auto title = title_id & ~uint64_t{0x800};
    uint32_t program = UINT32_MAX;
    for (uint32_t i = 0; i < programs.size(); ++i)
        if ((programs[i].id & ~uint64_t{0x800}) == title && programs[i].kind == 0) {
            program = i;
            break;
        }
    std::vector<Nca> result;
    if (program == UINT32_MAX) return result; // Every program NCA is still checked against the title.
    uint32_t base = UINT32_MAX, update = UINT32_MAX;
    for (uint32_t i = 0; i < snapshots.size(); ++i) {
        const auto& s = snapshots[i];
        if (s.program_index != program) continue;
        if (s.type == 0 && base == UINT32_MAX) base = i;
        if (s.type == 1 && (update == UINT32_MAX || s.numeric_version > snapshots[update].numeric_version)) update = i;
    }
    std::set<std::string> seen;
    for (const auto snapshot : {base, update})
        if (snapshot != UINT32_MAX) impl->Contents(snapshot, seen, result);
    return result;
}

void NxGame::Read(uint32_t file, uint64_t offset, std::span<char> bytes) {
    std::scoped_lock lock(impl->mutex);
    constexpr size_t Chunk = 8 * 1024 * 1024; // Below the reader's per-call limit.
    for (size_t done = 0; done < bytes.size();) {
        const auto length = std::min(Chunk, bytes.size() - done);
        const auto status = impl->archive->read_file(nx::FileId{file}, offset + done, length,
                                                     reinterpret_cast<uint8_t*>(bytes.data() + done), length);
        if (!status) throw std::runtime_error("Cannot read the NX file: " + status.error.message);
        done += length;
    }
}
} // namespace VideoCommon::ShaderProfile
