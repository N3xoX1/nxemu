// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <pthread.h>

#include "yuzu_common/common_types.h"
#include "yuzu_common/unique_function.h"
#include "yuzu_shader_recompiler/backend/bindings.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_shader_recompiler/stage.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/fixed_pipeline_state.h"

namespace Metal {

class Device;

/// Buffer indices used OUTSIDE the argument buffer: the argument buffer itself, the
/// vertex streams, the index buffer. Not the argument buffer's interior, SPIRV-Cross
/// assigns those slots. See mtl_argument_buffer.h for that contract.
struct BindingMap {
    /// Index 0 is the argument buffer. Guest uniform and storage buffers live
    /// inside it instead of taking slots of their own.
    static constexpr u32 ARGUMENT_BUFFER_INDEX = 0;
    /// Metal gives us 31 buffer argument slots (0..30). Hard limit, setBufferIndex
    /// past it is an assertion failure, not a warning.
    static constexpr u32 MAX_BUFFER_SLOTS = 31;
    static constexpr u32 VERTEX_BUFFER_BASE = 1;
    /// Fragment stage only, no vertex streams there: the recompiler's push constant
    /// block (GraphicsPushConstants: RescalingLayout + RenderAreaLayout).
    static constexpr u32 FRAGMENT_PUSH_CONSTANT_INDEX = 1;
    /// Fragment stage DescriptorSet 1 argument buffer (direct texture/sampler tables).
    static constexpr u32 FRAGMENT_TABLE_ARGUMENT_BUFFER_INDEX = 2;
    /// Compute stage push constant block (RescalingLayout). Slot 0 is the argument
    /// buffer; compute stages have no vertex streams.
    static constexpr u32 COMPUTE_PUSH_CONSTANT_INDEX = 1;
    /// Compute stage DescriptorSet 1 argument buffer (direct texture/sampler tables).
    static constexpr u32 COMPUTE_TABLE_ARGUMENT_BUFFER_INDEX = 2;
    /// Vertex stage push constant block (GraphicsPushConstants: RescalingLayout +
    /// RenderAreaLayout for PositionPass when viewport transform is disabled).
    static constexpr u32 VERTEX_PUSH_CONSTANT_INDEX = MAX_BUFFER_SLOTS - 1;
    /// Vertex stage DescriptorSet 1 argument buffer (direct texture/sampler tables).
    static constexpr u32 VERTEX_TABLE_ARGUMENT_BUFFER_INDEX = MAX_BUFFER_SLOTS - 2;
    /// Highest slot a vertex stream may use.
    static constexpr u32 MAX_VERTEX_BUFFER_SLOT = MAX_BUFFER_SLOTS - 3;

    /// Unused, nothing reads these.
    u32 uniform_buffer_base{1};
    u32 storage_buffer_base{8};
    u32 texture_base{0};
    u32 image_base{64};
    u32 sampler_base{0};
};

struct MslShader {
    std::string source;
    /// Entry point name SPIRV-Cross emitted. Not always "main", MSL reserves some
    /// identifiers and SPIRV-Cross renames on collision.
    std::string entry_point;
    Shader::Stage stage{};
    /// Number of `[[id(N)]]` slots SPIRV-Cross emitted in this stage's argument
    /// buffer struct. We write those in append order and Metal checks nothing, so
    /// comparing counts is the only guard we have. Catches a missing or extra
    /// resource, not a permutation.
    u32 argument_buffer_slots{};
    /// Number of `[[id(N)]]` slots SPIRV-Cross emitted in DescriptorSet 1's table
    /// argument buffer (direct texture/sampler tables), or 0 when unused.
    u32 table_argument_buffer_slots{};
};

/// Compact subset of Shader::Info retained on compiled pipelines after MSL
/// translation finishes. Shader::Info embeds large static_vectors for every
/// descriptor kind (~15 KiB per stage); replacing them with exact-sized
/// std::vectors cuts per-pipeline host RAM dramatically.
struct CompactStageInfo {
    std::vector<Shader::StorageBufferDescriptor> storage_buffers_descriptors;
    std::vector<Shader::TextureBufferDescriptor> texture_buffer_descriptors;
    std::vector<Shader::ImageBufferDescriptor> image_buffer_descriptors;
    std::vector<Shader::TextureDescriptor> texture_descriptors;
    std::vector<Shader::ImageDescriptor> image_descriptors;
    std::array<bool, 8> stores_frag_color{};
    std::array<bool, 8> stores_frag_alpha{};
    u32 constant_buffer_mask{};
    u32 input_attachment_mask{};
    u32 runtime_sampler_count{};
    u32 runtime_image_write_mask_words{};
    u32 write_tracking_slots{};
    bool stores_frag_depth{};
    bool uses_render_area{};
    bool uses_rescaling_uniform{};
    bool uses_typeless_image_writes{};
    bool uses_global_memory{};
    bool stores_global_memory{};

    CompactStageInfo() = default;
    explicit CompactStageInfo(const Shader::Info& src)
        : storage_buffers_descriptors(src.storage_buffers_descriptors.begin(),
                                      src.storage_buffers_descriptors.end()),
          texture_buffer_descriptors(src.texture_buffer_descriptors.begin(),
                                     src.texture_buffer_descriptors.end()),
          image_buffer_descriptors(src.image_buffer_descriptors.begin(),
                                   src.image_buffer_descriptors.end()),
          texture_descriptors(src.texture_descriptors.begin(), src.texture_descriptors.end()),
          image_descriptors(src.image_descriptors.begin(), src.image_descriptors.end()),
          stores_frag_color{src.stores_frag_color}, stores_frag_alpha{src.stores_frag_alpha},
          constant_buffer_mask{src.constant_buffer_mask},
          input_attachment_mask{src.input_attachment_mask},
          runtime_sampler_count{src.runtime_sampler_count},
          runtime_image_write_mask_words{src.runtime_image_write_mask_words},
          write_tracking_slots{src.write_tracking_slots}, stores_frag_depth{src.stores_frag_depth},
          uses_render_area{src.uses_render_area},
          uses_rescaling_uniform{src.uses_rescaling_uniform},
          uses_typeless_image_writes{src.uses_typeless_image_writes},
          uses_global_memory{src.uses_global_memory},
          stores_global_memory{src.stores_global_memory} {}
};

/// Translates a SPIR-V module to MSL. Throws Exception when SPIRV-Cross can't
/// translate it at all.
[[nodiscard]] MslShader TranslateSpirvToMsl(const Device& device,
                                            std::span<const u32> spirv,
                                            Shader::Stage stage,
                                            const Shader::Info& info,
                                            const BindingMap& bindings,
                                            bool rasterizes_points,
                                            bool d24_frag_depth_equal = false);

/// Translates a vertex program into a compute kernel writing its outputs to a
/// buffer, the only way Metal does transform feedback. Result is a Compute shader
/// even though the guest program is a vertex one. The captured layout is
/// SPIRV-Cross's own stage-out struct, not the guest's transform feedback layout, so
/// a gather step still has to move each varying to the buffer, offset and stride the
/// guest asked for.
[[nodiscard]] MslShader TranslateVertexToCaptureKernel(const Device& device,
                                                       std::span<const u32> spirv,
                                                       const Shader::Info& info,
                                                       const BindingMap& bindings,
                                                       const FixedPipelineState& state);

/// Buffer slot SPIRV-Cross writes a capture kernel's outputs to. Matches
/// CompilerMSL::Options::shader_output_buffer_index, which we set explicitly instead
/// of trusting the default to stay this value.
constexpr u32 CAPTURE_OUTPUT_BUFFER_INDEX = 28;

/// Buffer slot the capture dispatch's flattened index buffer takes. The injected
/// manual fetch reads it directly as `spvOrder`.
constexpr u32 CAPTURE_INDEX_BUFFER_INDEX = 29;

/// Buffer slot holding each vertex stream's size in bytes. Our hand-written
/// attribute fetch (Metal 4 has no stage-in for kernels) indexes raw memory by a
/// guest-supplied index and nothing else bounds it. An unchecked read past a stream
/// can fault the GPU and kill the queue.
constexpr u32 CAPTURE_LIMITS_BUFFER_INDEX = 30;

/// Index into the capture limits buffer of the draw's base vertex, followed by its base
/// instance. Past the per-stream limits.
constexpr u32 CAPTURE_DRAW_BASE_SLOT = BindingMap::MAX_BUFFER_SLOTS;

/// Whether slot is one of the three the capture dispatch keeps for itself. All three
/// sit inside the range vertex streams bind from, so we name them one by one.
/// Treating everything above the output slot as reserved would drop the
/// constant-attribute binding, which uses the last legal slot on purpose.
[[nodiscard]] constexpr bool IsCaptureReservedBufferSlot(u32 slot) noexcept {
    return slot == CAPTURE_OUTPUT_BUFFER_INDEX || slot == CAPTURE_INDEX_BUFFER_INDEX ||
           slot == CAPTURE_LIMITS_BUFFER_INDEX;
}

/// Compiles MSL source into a Metal library. Returns null and logs on failure.
[[nodiscard]] Owned<MTL::Library> BuildLibrary(const Device& device, const MslShader& shader);

/// Clears cached Metal libraries (for a specific device registry ID, or all when 0)
/// so compiled libraries do not retain MTL::Device across game sessions.
void ClearLibraryCache(u64 device_registry_id = 0);

/// Looks up the entry point in the library and returns the function object.
[[nodiscard]] Owned<MTL::Function> GetFunction(MTL::Library* library,
                                                const std::string& entry_point);

/// Declares one colour attachment on a Metal 4 render pipeline descriptor. MTL4
/// attachments are set as objects, not mutated in place, so this wraps the
/// alloc/set/release for callers that only have a format.
void SetColorAttachment(MTL4::RenderPipelineDescriptor* descriptor, u32 index,
                        MTL::PixelFormat format);

/// Names entry_point in library for a Metal 4 pipeline descriptor. Returns an
/// autoreleased descriptor, wrapped in a specialization whenever the function
/// declares ANY function constants. Metal 4 never sees the MTL::Function GetFunction
/// specializes, so we have to do it here too.
[[nodiscard]] MTL4::FunctionDescriptor* MakeFunctionDescriptor(MTL::Library* library,
                                                               const std::string& entry_point);

/// Builds a compute pipeline through MTL4::Compiler for helper kernels with no other
/// pipeline state to configure. Metal 4 has no newComputePipelineState(function)
/// overload so even those need a descriptor. Passes the device's lookup options, a
/// kernel compiled on a previous run is an archive lookup instead of a compile.
/// Returns null and logs on failure.
[[nodiscard]] Owned<MTL::ComputePipelineState> BuildComputePipeline(const Device& device,
                                                                    MTL::Library* library,
                                                                    const std::string& entry_point,
                                                                    const char* label);

/// Stack for SPIR-V translation. SPIRV-Cross recurses deep over the control flow
/// graph and overflows the 512 KiB an Apple secondary thread gets by default, and
/// translation always runs on a worker.
constexpr size_t SPIRV_CROSS_STACK_SIZE = 8 * 1024 * 1024;

/// Runs translate on a thread with a SPIRV_CROSS_STACK_SIZE stack.
/// If the calling thread already has an >= SPIRV_CROSS_STACK_SIZE stack (such as
/// ShaderWorkerPool workers), runs inline without a thread hop.
[[nodiscard]] MslShader RunOnLargeStack(std::function<MslShader()> translate);

/// Thread pool whose workers are created with a SPIRV_CROSS_STACK_SIZE (8 MiB) stack
/// so SPIRV-Cross translation runs inline with zero helper-thread context switches.
class ShaderWorkerPool {
public:
    explicit ShaderWorkerPool(size_t num_workers, std::string name);
    ~ShaderWorkerPool();

    ShaderWorkerPool(const ShaderWorkerPool&) = delete;
    ShaderWorkerPool& operator=(const ShaderWorkerPool&) = delete;

    void QueueWork(Common::UniqueFunction<void> work, bool high_priority = false);
    void WaitForRequests(std::stop_token stop_token = {});

    /// True when no task is queued or actively running on any worker.
    [[nodiscard]] bool IsIdle() const noexcept {
        return active_or_queued.load(std::memory_order_acquire) == 0;
    }

private:
    static void* WorkerEntry(void* raw);
    void WorkerLoop();

    std::string thread_name;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::condition_variable wait_cv;
    std::deque<Common::UniqueFunction<void>> requests;
    std::vector<pthread_t> threads;
    std::atomic<size_t> active_or_queued{0};
    size_t work_scheduled{};
    size_t work_done{};
    size_t workers_stopped{};
    size_t workers_total{};
    bool stop_requested{false};
};

} // namespace Metal
