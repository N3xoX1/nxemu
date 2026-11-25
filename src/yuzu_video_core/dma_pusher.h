// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <queue>

#include "yuzu_common/bit_field.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/scratch_buffer.h"
#include "yuzu_video_core/command_header.h"
#include "yuzu_video_core/engines/engine_interface.h"
#include "yuzu_video_core/engines/puller.h"

namespace Core {
class System;
}

namespace Tegra {

namespace Control {
struct ChannelState;
}

class GPU;
class MemoryManager;

// Note that, traditionally, methods are treated as 4-byte addressable locations, and hence
// their numbers are written down multiplied by 4 in Docs. Here we are not multiply by 4.
// So the values you see in docs might be multiplied by 4.
// Register documentation:
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/classes/host/cla26f.h
//
// Register Description (approx):
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/manuals/volta/gv100/dev_pbdma.ref.txt
enum class BufferMethods : u32 {
    BindObject = 0x0,
    Illegal = 0x1,
    Nop = 0x2,
    SemaphoreAddressHigh = 0x4,
    SemaphoreAddressLow = 0x5,
    SemaphoreSequencePayload = 0x6,
    SemaphoreOperation = 0x7,
    NonStallInterrupt = 0x8,
    WrcacheFlush = 0x9,
    MemOpA = 0xA,
    MemOpB = 0xB,
    MemOpC = 0xC,
    MemOpD = 0xD,
    RefCnt = 0x14,
    SemaphoreAcquire = 0x1A,
    SemaphoreRelease = 0x1B,
    SyncpointPayload = 0x1C,
    SyncpointOperation = 0x1D,
    WaitForIdle = 0x1E,
    CRCCheck = 0x1F,
    Yield = 0x20,
    NonPullerMethods = 0x40,
};

struct CommandListHeader {
    union {
        u64 raw;
        BitField<0, 1, u64> conditional_fetch;
        BitField<40, 1, u64> is_privileged;
        BitField<41, 1, u64> is_non_main;
        BitField<42, 21, u64> size;
        BitField<63, 1, u64> sync;
    };

    [[nodiscard]] GPUVAddr Address() const {
        // GP_ENTRY0 bits 0-1 are not part of the byte address.
        return static_cast<GPUVAddr>(raw & 0xFFFFFFFFFCULL);
    }
};
static_assert(sizeof(CommandListHeader) == sizeof(u64), "CommandListHeader is incorrect size");

inline CommandHeader BuildCommandHeader(BufferMethods method, u32 arg_count, SubmissionMode mode) {
    CommandHeader result{};
    result.method.Assign(static_cast<u32>(method));
    result.arg_count.Assign(arg_count);
    result.mode.Assign(mode);
    return result;
}

struct CommandList final {
    CommandList() = default;
    explicit CommandList(std::size_t size) : command_lists(size) {}
    explicit CommandList(
        boost::container::small_vector<CommandHeader, 512>&& prefetch_command_list_)
        : prefetch_command_list{std::move(prefetch_command_list_)} {}

    boost::container::small_vector<CommandListHeader, 512> command_lists;
    boost::container::small_vector<CommandHeader, 512> prefetch_command_list;
};

/**
 * The DmaPusher class implements DMA submission to FIFOs, providing an area of memory that the
 * emulated app fills with commands and tells PFIFO to process. The pushbuffers are then assembled
 * into a "command stream" consisting of 32-bit words that make up "commands".
 * See https://envytools.readthedocs.io/en/latest/hw/fifo/dma-pusher.html#fifo-dma-pusher for
 * details on this implementation.
 */
class DmaPusher final {
public:
    explicit DmaPusher(GPU& gpu_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_);
    ~DmaPusher();

    void Push(CommandList&& entries) {
        dma_pushbuffer.push(std::move(entries));
    }

    void DispatchCalls();

    void BindSubchannel(Engines::EngineInterface* engine, u32 subchannel_id,
                        Engines::EngineTypes engine_type) {
        FlushFrontSink(subchannel_id);
        subchannels[subchannel_id] = engine;
        subchannel_type[subchannel_id] = engine_type;
        if (engine_type == Engines::EngineTypes::Maxwell3D) {
            maxwell3d_engine = engine;
        }
    }

    void BindRasterizer(VideoCore::RasterizerInterface* rasterizer);

private:
    static constexpr u32 non_puller_methods = 0x40;
    static constexpr u32 max_subchannels = 8;
    enum class MemoryOperationSyncState : u8 {
        None,
        Skip,
        Armed,
    };

    bool Step();
    void SynchronizeMemoryOperations();
    void ArmMemoryOperationSync();
    void WaitForArmedMemoryOperationSync();
    bool CanSkipCleanMemoryOperationSync(const CommandListHeader& header) const;
    void ProcessCommands(std::span<const CommandHeader> commands);

    void SetState(const CommandHeader& command_header);

    /// Hands the queued Maxwell3D methods to the GPU thread
    void DrainMaxwell3D() const;
    void DrainMaxwell3DMemoryWrites(GPUVAddr address, size_t size) const;
    bool IsAcquireMet(u32 method, u32 argument) const;
    void UpdateUniformPrefetchAtAcquire(u32 method, u32 argument) const;
    void ResumeUniformPrefetch() const;

    /// True when a decode thread runs this pusher
    bool IsThreaded() const;
    /// Guest memory a push for the GPU thread may write, see BackQueue::MayWrite
    struct BackWrites {
        enum class Kind { Anywhere, Range, Nothing };
        Kind kind{Kind::Anywhere};
        GPUVAddr address{};
        u64 size{};
        /// A second range, such as the semaphore a copy releases
        GPUVAddr extra_address{};
        u64 extra_size{};
    };

    /// Runs `work` on the GPU thread after the queued Maxwell3D methods
    void PostBack(std::function<void()>&& work) const;
    void PostBack(std::function<void()>&& work, BackWrites writes) const;
    BackWrites PullerWrites(u32 method, std::span<const u32> arguments) const;
    BackWrites EngineWrites(u32 subchannel_id, u32 method) const;
    BackWrites UploadWrites(u32 subchannel_id) const;
    BackWrites CopyWrites() const;
    BackWrites BlitWrites() const;
    static u64 BlockLinearSize(u64 line_bytes, u32 height, u32 depth, u32 block_height,
                               u32 block_depth);
    void TrackFrontRegister(u32 subchannel_id, u32 method, u32 argument) const;
    void ApplyFrontSink(u32 subchannel_id) const;
    /// Records the pages of a fetched entry, true when one was never fetched before
    bool FetchesNewPages(DAddr address, u64 size) const;
    /// Decode thread, after reading an entry: keeps it while dispatches launched before it may
    /// still be queued
    void RecordUnwaitedEntry(GPUVAddr address, size_t size) const;
    /// True when an entry read while dispatch `id` was queued now lies in compute written pages
    bool UnwaitedEntryWritten(u64 id) const;
    /// GPU thread, after dispatch `id` ran
    void DispatchRan(u64 id) const;
    bool IsKnownPushbuffer(GPUVAddr address, size_t size) const;
    /// Records the entry pages, true when a queued dispatch may have written the entry
    bool DispatchMayWrite(GPUVAddr address, size_t size,
                          std::span<const std::pair<DAddr, std::size_t>> ranges) const;
    std::vector<std::pair<u32, u32>>& SinkFor(u32 subchannel_id) const;
    void FlushFrontSink(u32 subchannel_id) const;

    void CallMethod(u32 argument) const;
    void CallMultiMethod(const u32* base_start, u32 num_methods) const;

    Common::ScratchBuffer<CommandHeader>
        command_headers; ///< Buffer for list of commands fetched at once

    std::queue<CommandList> dma_pushbuffer; ///< Queue of command lists to be processed
    std::size_t dma_pushbuffer_subindex{};  ///< Index within a command list within the pushbuffer

    struct DmaState {
        u32 method;            ///< Current method
        u32 subchannel;        ///< Current subchannel
        u32 method_count;      ///< Current method count
        u32 length_pending;    ///< Large NI command length pending
        GPUVAddr dma_get;      ///< Currently read segment
        u64 dma_word_offset;   ///< Current word offset from address
        bool non_incrementing; ///< Current command's NI flag
        bool is_last_call;
    };

    DmaState dma_state{};
    bool dma_increment_once{};
    MemoryOperationSyncState memory_operation_sync_state{MemoryOperationSyncState::None};
    std::atomic<bool> memory_operation_sync_ready{};

    const bool ib_enable{true}; ///< IB mode enabled

    std::array<Engines::EngineInterface*, max_subchannels> subchannels{};
    std::array<Engines::EngineTypes, max_subchannels> subchannel_type;
    Engines::EngineInterface* maxwell3d_engine{};
    /// Register writes for engines that run on the GPU thread, see SinkFor
    mutable std::array<std::vector<std::pair<u32, u32>>, max_subchannels> front_sinks;
    /// Puller methods that only write a register, sent with the next puller method that acts
    mutable std::vector<Engines::Puller::MethodCall> pending_puller;
    mutable std::array<u32, static_cast<u32>(BufferMethods::NonPullerMethods)> front_puller_regs{};
    mutable bool unmet_acquire_seen{};
    /// A compute dispatch may still be queued for the GPU thread
    mutable bool queued_dispatch{};
    /// Dispatches launched by the decode thread without a wait, numbered from 1
    mutable u64 launched_dispatches{};
    /// The dispatch the method being posted launches, 0 for none
    mutable u64 posted_dispatch{};
    /// The last dispatch the GPU thread ran
    mutable std::atomic<u64> completed_dispatches{};
    /// Entries read while dispatches first..last were queued, see DispatchRan. Atomic fields:
    /// the GPU thread reads them while the decode thread records more.
    struct UnwaitedEntry
    {
        std::atomic<u64> address{};
        std::atomic<u64> size{};
        std::atomic<u64> first{};
        std::atomic<u64> last{};
    };
    static constexpr size_t UnwaitedEntryCount = 2048;
    mutable std::array<UnwaitedEntry, UnwaitedEntryCount> unwaited_entries{};
    mutable std::atomic<u64> unwaited_count{};
    /// GPU thread: the compute write epoch at the last dispatch that ran
    mutable u64 checked_epoch{};
    /// A dispatch wrote commands read before it ran: every dispatch is waited for
    mutable std::atomic<bool> strict_dispatch_wait{};
    /// Device pages fetched as pushbuffer entries, see FetchesNewPages
    static constexpr unsigned FetchedAddressBits = 34;
    static constexpr unsigned FetchedPageBits = 12;
    mutable std::vector<u64> fetched_pages =
        std::vector<u64>((u64{1} << (FetchedAddressBits - FetchedPageBits)) / 64);
    /// GPU pages fetched before that no dispatch wrote, valid while the mapping and the
    /// dispatch written pages stay the same, see IsKnownPushbuffer
    struct KnownPage
    {
        u64 page{~u64{0}};
        u64 mapping_generation{};
        u64 compute_write_epoch{};
    };
    static constexpr size_t KnownPageCount = 1024;
    mutable std::vector<KnownPage> known_pages = std::vector<KnownPage>(KnownPageCount);
    /// Inline to memory registers of KeplerCompute and KeplerMemory, as the engines will have them
    mutable std::array<std::array<u32, 13>, 2> front_upload{};
    /// MaxwellDMA and Fermi2D registers, as the engines will have them when they run
    mutable std::array<u32, 0x800> front_copy{};
    mutable std::array<u32, 0x258> front_blit{};

    GPU& gpu;
    MemoryManager& memory_manager;
    mutable Engines::Puller puller;

    VideoCore::RasterizerInterface* rasterizer{};
};

} // namespace Tegra
