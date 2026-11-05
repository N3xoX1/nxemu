// SPDX-FileCopyrightText: Copyright 2026 nxemu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <vector>

#include "yuzu_video_core/buffer_cache/uniform_stream_reuse.h"
#include "yuzu_video_core/texture_cache/image_view_descriptor_index.h"

namespace {

struct Memory {
    std::array<u8, 8192> bytes{};
    u32 reads{};

    void ReadBlockUnsafe(u64 address, void* dst, size_t size) {
        assert(address <= bytes.size() && size <= bytes.size() - address);
        ++reads;
        std::memcpy(dst, bytes.data() + address, size);
    }
};

struct Runtime {
    struct StreamUniformRef {
        size_t allocation{};
    };
    u64 tick{1};
    std::vector<std::vector<u8>> allocations;
    std::vector<StreamUniformRef> descriptors;

    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        allocations.emplace_back(size);
        const StreamUniformRef ref{allocations.size() - 1};
        descriptors.push_back(ref);
        return allocations.back();
    }

    StreamUniformRef LastStreamUniformRef() const { return descriptors.back(); }
    void RebindStreamUniform(StreamUniformRef ref, u32 size) {
        assert(ref.allocation < allocations.size());
        assert(allocations[ref.allocation].size() == size);
        descriptors.push_back(ref);
    }
    u64 CurrentTick() const { return tick; }
};

struct UnsupportedRuntime {};

static_assert(sizeof(VideoCommon::UniformStreamReuse<Runtime>) < 512 * 1024);
static_assert(sizeof(VideoCommon::UniformStreamReuse<UnsupportedRuntime>) == 1);

void TestUniformReuse() {
    using VideoCommon::UniformStreamReuse;
    Memory memory;
    Runtime runtime;
    UniformStreamReuse<Runtime> reuse;
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 1);

    // Same bytes at another guest address reuse the same staging allocation.
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 200, 128));
    assert(runtime.allocations.size() == 1);
    assert(runtime.descriptors.back().allocation == 0);

    // Each changed value needs its own snapshot. After two misses, avoid more memcmps.
    memory.bytes[0] = 1;
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 2);
    memory.bytes[0] = 2;
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 3);
    memory.bytes[0] = 3;
    assert(!reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 3);
    const auto fallback = runtime.BindMappedUniformBuffer(0, 0, 128);
    memory.ReadBlockUnsafe(0, fallback.data(), fallback.size());
    assert(runtime.allocations.size() == 4 && runtime.allocations.back()[0] == 3);

    // A submitted command buffer changes the tick: no stale descriptor may be reused.
    ++runtime.tick;
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 5);
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 5);

    // Different stages/sizes never share snapshots by accident.
    assert(reuse.Bind(runtime, memory, 1, 0, 0, 0, 128));
    assert(runtime.allocations.size() == 6);
    assert(reuse.Bind(runtime, memory, 0, 0, 0, 0, 64));
    assert(runtime.allocations.size() == 7);

    // A newly created buffer cache starts empty, even on an identical runtime tick.
    UniformStreamReuse<Runtime> second_cache;
    assert(second_cache.Bind(runtime, memory, 0, 0, 0, 0, 64));
    assert(runtime.allocations.size() == 8);

    const auto count = runtime.allocations.size();
    assert(!reuse.Bind(runtime, memory, 0, 0, 0, 0, 4097));
    assert(!reuse.Bind(runtime, memory, 0, 18, 0, 0, 16));
    assert(runtime.allocations.size() == count);
    UniformStreamReuse<UnsupportedRuntime> unsupported;
    UnsupportedRuntime other;
    assert(!unsupported.Bind(other, memory, 0, 0, 0, 0, 16));

#ifdef _WIN32
    _putenv_s("NXEMU_UNIFORM_STREAM_REUSE", "0");
#else
    setenv("NXEMU_UNIFORM_STREAM_REUSE", "0", 1);
#endif
    UniformStreamReuse<Runtime> disabled;
    assert(!disabled.Bind(runtime, memory, 0, 0, 0, 0, 128));
#ifdef _WIN32
    _putenv_s("NXEMU_UNIFORM_STREAM_REUSE", "");
#else
    unsetenv("NXEMU_UNIFORM_STREAM_REUSE");
#endif
}

void TestDescriptorReverseIndex() {
    VideoCommon::ImageViewDescriptorIndex<u32, u32> index;
    std::array<std::unordered_map<u32, u32>, 3> channels;
    const auto register_view = [&](size_t channel, u32 descriptor, u32 view) {
        const auto [it, inserted] = channels[channel].emplace(descriptor, view);
        assert(inserted);
        index.Register(view, channel, it->first);
    };
    register_view(0, 10, 5);
    register_view(0, 11, 5);
    register_view(1, 12, 5);
    register_view(1, 13, 6);
    register_view(2, 14, 7);

    const auto remove_view = [&](u32 view) {
        index.Invalidate(std::span<const u32>{&view, 1}, [&](size_t channel, u32 descriptor, u32 removed) {
            auto& entries = channels[channel];
            auto it = entries.find(descriptor);
            if (it != entries.end() && it->second == removed) {
                entries.erase(it);
            }
        });
    };
    remove_view(5);
    assert(channels[0].empty());
    assert(channels[1].size() == 1 && channels[1].at(13) == 6);
    assert(channels[2].at(14) == 7);

    // The slot vector may recycle a removed view ID for a new descriptor.
    register_view(0, 15, 5);
    remove_view(5);
    assert(channels[0].empty());

    // Channel recycling must not leave references to the next channel's descriptors.
    index.ForgetChannel(1, channels[1]);
    channels[1].clear();
    channels[1].emplace(13, 42);
    remove_view(6);
    assert(channels[1].at(13) == 42);

    // Guard against a changed mapping even if its old reverse reference remains.
    channels[2][14] = 99;
    remove_view(7);
    assert(channels[2].at(14) == 99);

    // Dense/sparse mixtures and a many-view GC batch must match a full scan.
    for (u32 descriptor = 0; descriptor < 2000; ++descriptor) {
        const size_t channel = descriptor % channels.size();
        const u32 view = 100 + descriptor % 73;
        const auto [it, inserted] = channels[channel].emplace(1000 + descriptor, view);
        assert(inserted);
        index.Register(view, channel, it->first);
    }
    std::vector<u32> removed;
    for (u32 id = 100; id < 173; id += 3) {
        removed.push_back(id);
    }
    assert(index.CountReferences(removed) > 0);
    index.Invalidate(removed, [&](size_t channel, u32 descriptor, u32 view) {
        auto& entries = channels[channel];
        auto it = entries.find(descriptor);
        if (it != entries.end() && it->second == view) {
            entries.erase(it);
        }
    });
    for (const auto& entries : channels) {
        for (const auto& [descriptor, view] : entries) {
            assert(std::find(removed.begin(), removed.end(), view) == removed.end());
        }
    }
    assert(index.CountReferences(removed) == 0);

    // The dense fallback deletes views with one forward scan, then only drops reverse keys.
    std::array<std::unordered_map<u32, u32>, 2> dense;
    VideoCommon::ImageViewDescriptorIndex<u32, u32> dense_index;
    for (u32 descriptor = 0; descriptor < 1000; ++descriptor) {
        const size_t channel = descriptor % dense.size();
        const u32 view = descriptor < 900 ? 200 : 201;
        const auto [it, inserted] = dense[channel].emplace(descriptor, view);
        assert(inserted);
        dense_index.Register(view, channel, it->first);
    }
    const u32 deleted = 200;
    assert(dense_index.HasDenseViews(std::span<const u32>{&deleted, 1}));
    assert(dense_index.CountReferences(std::span<const u32>{&deleted, 1}) == 0);
    for (auto& entries : dense) {
        std::erase_if(entries, [](const auto& item) { return item.second == 200; });
        assert(entries.size() == 50);
    }
    dense_index.ForgetViews(std::span<const u32>{&deleted, 1});
    assert(dense_index.CountReferences(std::span<const u32>{&deleted, 1}) == 0);
}

} // namespace

int main() {
    TestUniformReuse();
    TestDescriptorReverseIndex();
}
