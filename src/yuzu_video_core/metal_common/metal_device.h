// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

/// Coarse hardware bucket derived from the Metal GPU family. Apple parts are
/// tile-based with unified memory, discrete Mac GPUs are immediate-mode with
/// dedicated VRAM, and storage-mode policy hinges on that split.
enum class DeviceClass {
    AppleSilicon, ///< Apple7+ family: M-series and A-series
    AppleMobile,  ///< Apple1-Apple6: older iOS hardware
    MacDiscrete,  ///< Mac2 family without Apple family support (AMD/Intel)
    Unknown,
};

/// Static description of a selectable Metal device, for the settings UI. Built
/// without creating a renderer so the configuration dialog can enumerate GPUs.
struct DeviceRecord {
    std::string name;
    u64 registry_id{};
    bool low_power{};
    bool headless{};
};

/// Enumerates every Metal device the system exposes. On iOS there is exactly
/// one, on macOS there may be an integrated and a discrete GPU, plus eGPUs.
[[nodiscard]] std::vector<DeviceRecord> EnumerateDevices();

/// Owns the MTL::Device and the queue everything is submitted through, and
/// answers the capability questions the rest of the backend asks.
class Device : Common::NonCopyable, Common::NonMoveable {
public:
    /// @param device_index Index into EnumerateDevices(). Out of range values
    ///                     fall back to the system default device.
    explicit Device(u32 device_index);
    ~Device();

    [[nodiscard]] MTL::Device* GetDevice() const noexcept {
        return device.get();
    }

    [[nodiscard]] MTL::CommandQueue* GetCommandQueue() const noexcept {
        return command_queue.get();
    }

    [[nodiscard]] const std::string& GetDeviceName() const noexcept {
        return device_name;
    }

    /// Vendor string shown in the title bar and logs.
    [[nodiscard]] std::string GetVendorName() const;

    [[nodiscard]] DeviceClass GetDeviceClass() const noexcept {
        return device_class;
    }

    [[nodiscard]] bool IsTileBased() const noexcept {
        return device_class == DeviceClass::AppleSilicon ||
               device_class == DeviceClass::AppleMobile;
    }

    /// True on iOS/tvOS. Distinct from IsTileBased: an Apple Silicon Mac is
    /// tile based but not a mobile platform.
    [[nodiscard]] bool IsMobile() const noexcept {
        return mobile_platform;
    }

    /// True when CPU and GPU share one address space, so Shared storage stages
    /// without a copy and Managed is unnecessary (and unavailable on iOS).
    [[nodiscard]] bool HasUnifiedMemory() const noexcept {
        return unified_memory;
    }

    /// Tier 2 argument buffers, required to hand-write resource handles into an
    /// argument buffer. A hard requirement; construction throws without it.
    [[nodiscard]] bool SupportsArgumentBuffersTier2() const noexcept {
        return argument_buffers_tier2;
    }

    /// Metal 3 feature set. Gates the MSL 3.0 language version.
    [[nodiscard]] bool SupportsMetal3() const noexcept {
        return metal3;
    }

    /// Metal 4 is a separate encoding model, not a newer Metal 3. Binding goes
    /// through argument tables, residency is explicit, and there is no automatic
    /// hazard tracking, every dependency has to be an explicit barrier. A hard
    /// requirement, construction throws without it.
    [[nodiscard]] bool SupportsMetal4() const noexcept {
        return metal4;
    }

    /// MTL4 command queue. Null unless SupportsMetal4().
    [[nodiscard]] MTL4::CommandQueue* GetMTL4Queue() const noexcept {
        return mtl4_queue.get();
    }

    /// Replaces the MTL4 queue with a fresh one, and returns whether it worked.
    /// A queue that has taken enough GPU errors stops executing anything. Every
    /// later commit comes back Code=5 "Ignored (for causing prior/excessive GPU
    /// errors)" having run for 0.00 ms and it never recovers on its own. One run
    /// took four page faults and then ignored 7740 submissions in a row, which is
    /// what turns a fault into a dead emulator instead of a dropped frame.
    /// Nothing else clears that state, so we rebuild the queue.
    ///
    /// Only safe to call from the thread that submits, with nothing in flight
    /// that still needs the old queue. Work already committed to it is gone
    /// either way, the queue was ignoring it.
    [[nodiscard]] bool RecreateMTL4Queue() const;

    /// Called after the queue is replaced. Anything holding state that belonged
    /// to the old queue has to drop it here, the swapchain in particular, whose
    /// outstanding drawable wait died with that queue and whose matching signal
    /// would otherwise go to a queue that never waited.
    void SetQueueRebuiltHandler(std::function<void()> handler) const {
        on_queue_rebuilt = std::move(handler);
    }

    /// Compiles render pipelines this backend builds. Null unless SupportsMetal4().
    [[nodiscard]] MTL4::Compiler* GetMTL4Compiler() const noexcept {
        return mtl4_compiler.get();
    }

    /// Compiles compute pipelines this backend builds, backed by its own binary
    /// archive serializer so compute pipelines never mix with graphics pipelines.
    [[nodiscard]] MTL4::Compiler* GetMTL4ComputeCompiler() const noexcept {
        return mtl4_compute_compiler.get();
    }

    /// Unserialized Metal 4 compiler for internal/third-party helpers (e.g. MetalFX)
    /// that may mix render and compute pipelines internally.
    [[nodiscard]] MTL4::Compiler* GetMTL4UtilityCompiler() const noexcept {
        return mtl4_utility_compiler.get();
    }

    /// Compiler task options carrying graphics archives from previous runs to look
    /// pipelines up in, turning a compile into a lookup. Null when there are none.
    [[nodiscard]] MTL4::CompilerTaskOptions* GetMTL4LookupOptions() const noexcept {
        return mtl4_lookup_options.get();
    }

    /// Compiler task options carrying compute archives from previous runs.
    [[nodiscard]] MTL4::CompilerTaskOptions* GetMTL4ComputeLookupOptions() const noexcept {
        return mtl4_compute_lookup_options.get();
    }

    /// Thread-safe wrappers around MTL4::Compiler pipeline creation. Graphics and compute
    /// each have their own shared_mutex so workers compile in parallel with zero
    /// cross-stage contention while SaveMetal4Archive() takes an exclusive lock only on
    /// the serializer it is currently flushing (MTL4PipelineDataSetSerializer::
    /// serializeAsArchiveAndFlushToURL is not safe concurrently with active compilation).
    [[nodiscard]] Owned<MTL::RenderPipelineState> NewRenderPipelineState(
        const MTL4::PipelineDescriptor* descriptor, NS::Error** error) const;
    [[nodiscard]] Owned<MTL::ComputePipelineState> NewComputePipelineState(
        const MTL4::ComputePipelineDescriptor* descriptor, NS::Error** error) const;

    /// Loads any archive previously written for @p title_id and points the
    /// serializer at where this run's archive will be flushed.
    void LoadMetal4Archive(u64 title_id) const;

    /// Writes this run's compiled pipelines out. Called periodically when idle and
    /// unconditionally at shutdown.
    void SaveMetal4Archive(bool save_graphics = true, bool save_compute = true) const;

    /// Machine-wide residency set. Metal 4 makes nothing resident implicitly,
    /// so anything the GPU may touch has to be in one of these.
    [[nodiscard]] MTL::ResidencySet* GetResidencySet() const noexcept {
        return mtl4_residency_set.get();
    }

    /// Declares @p allocation as something the GPU may touch. Thread-safe. An
    /// access to a non-resident allocation reads unmapped memory with no
    /// validation error and no fault, so the failure mode is silent corruption.
    /// Additions are batched, only CommitResidency republishes the set.
    void AddResidentAllocation(const MTL::Allocation* allocation) const;

    /// Drops @p allocation from the residency set. Only call it once the GPU can
    /// no longer reach it, work already recorded may still touch it and dropping
    /// it early faults. Scheduler::SweepKeepAlive is that moment.
    void RemoveResidentAllocation(const MTL::Allocation* allocation) const;

    /// Publishes pending additions and removals. Committing a set is not free,
    /// so this runs once per submission rather than per allocation.
    void CommitResidency() const;

    [[nodiscard]] bool SupportsBCTextureCompression() const noexcept {
        return bc_texture_compression;
    }

    /// D24S8 is emulated on Apple GPUs, when absent we promote to D32S8.
    [[nodiscard]] bool SupportsDepth24Stencil8() const noexcept {
        return depth24_stencil8;
    }

    [[nodiscard]] bool Supports32BitFloatFiltering() const noexcept {
        return float32_filtering;
    }

    /// Required for GPU timestamp queries.
    [[nodiscard]] bool SupportsStageBoundaryCounters() const noexcept {
        return stage_boundary_counters;
    }

    [[nodiscard]] bool SupportsRasterOrderGroups() const noexcept {
        return raster_order_groups;
    }

    /// Min/max sampler reduction. Where it is absent `setReductionMode` does not
    /// degrade to an average, newSamplerState returns nothing at all. A null
    /// sampler is never bound, so the shader keeps sampling through the stale
    /// contents of its argument table slot.
    [[nodiscard]] bool SupportsSamplerReduction() const noexcept {
        return sampler_reduction;
    }

    /// Largest MSAA sample count the device accepts, clamped to 8.
    [[nodiscard]] u32 GetMaxSampleCount() const noexcept {
        return max_sample_count;
    }

    [[nodiscard]] u64 GetMaxBufferLength() const noexcept {
        return max_buffer_length;
    }

    [[nodiscard]] u64 GetMaxThreadgroupMemoryLength() const noexcept {
        return max_threadgroup_memory;
    }

    /// Budget hint in bytes, the texture cache uses this to size itself.
    [[nodiscard]] u64 GetRecommendedWorkingSetSize() const noexcept {
        return recommended_working_set;
    }

    /// SIMD group width. Inferred from the device class rather than queried,
    /// 32 on Apple GPUs and 64 on discrete Mac parts.
    [[nodiscard]] u32 GetSubgroupSize() const noexcept {
        return subgroup_size;
    }

    /// MSL version to ask SPIRV-Cross for, encoded as major * 10000 + minor * 100.
    [[nodiscard]] u32 GetMslVersion() const noexcept {
        return msl_version;
    }

    void LogDeviceInfo() const;

private:
    void DetectCapabilities();
    /// Creates the MTL4 queue, compiler and residency set. Throws if any of them
    /// cannot be made, there is no fallback path.
    void CreateMetal4Objects();

    Owned<MTL::Device> device;
    Owned<MTL::CommandQueue> command_queue;

    /// Mutable so RecreateMTL4Queue can run from the const references the
    /// scheduler and renderer hold.
    mutable Owned<MTL4::CommandQueue> mtl4_queue;
    mutable std::function<void()> on_queue_rebuilt;
    Owned<MTL4::Compiler> mtl4_compiler;
    Owned<MTL4::Compiler> mtl4_compute_compiler;
    Owned<MTL4::Compiler> mtl4_utility_compiler;
    /// Captures graphics and compute pipelines separately for serialization at exit.
    Owned<MTL4::PipelineDataSetSerializer> mtl4_serializer;
    Owned<MTL4::PipelineDataSetSerializer> mtl4_compute_serializer;
    /// Archives loaded from previous runs. Mutable because the pipeline cache
    /// holds the Device by const reference and these are established lazily
    /// once the title id is known.
    mutable Owned<MTL4::CompilerTaskOptions> mtl4_lookup_options;
    mutable Owned<MTL4::CompilerTaskOptions> mtl4_compute_lookup_options;
    mutable std::string mtl4_archive_path;
    mutable std::string mtl4_compute_archive_path;
    mutable std::mutex archive_mutex;
    mutable std::shared_mutex graphics_compiler_rw_mutex;
    mutable std::shared_mutex compute_compiler_rw_mutex;
    Owned<MTL::ResidencySet> mtl4_residency_set;
    struct PendingResidencyOp {
        MTL::Allocation* allocation;
        bool add;
    };
    /// Serializes CommitResidency() calls touching mtl4_residency_set.
    mutable std::mutex residency_mutex;
    /// Short-held mutex for staging adds/removals without blocking on commit().
    mutable std::mutex pending_residency_mutex;
    mutable std::vector<PendingResidencyOp> pending_residency_ops;
    /// Allocations removed from mtl4_residency_set whose commit() is deferred
    /// until the next addition or batch threshold, kept retained until commit().
    mutable std::vector<MTL::Allocation*> deferred_residency_releases;
    mutable bool residency_dirty{false};
    /// Only additions need wiring, a removal never does. See CommitResidency.
    mutable bool residency_needs_request{false};

    std::string device_name;
    DeviceClass device_class{DeviceClass::Unknown};

    bool mobile_platform{};
    bool unified_memory{};
    bool argument_buffers_tier2{};
    bool metal3{};
    bool metal4{};
    bool bc_texture_compression{};
    bool depth24_stencil8{};
    bool float32_filtering{};
    bool stage_boundary_counters{};
    bool raster_order_groups{};
    bool sampler_reduction{};

    u32 max_sample_count{1};
    u32 subgroup_size{32};
    u32 msl_version{};
    u64 max_buffer_length{};
    u64 max_threadgroup_memory{};
    u64 recommended_working_set{};
};

} // namespace Metal
