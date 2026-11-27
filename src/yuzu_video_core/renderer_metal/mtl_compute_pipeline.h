// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_video_core/buffer_cache/buffer_cache_base.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Metal {

class Device;
class Scheduler;

/// A compiled compute kernel, built asynchronously on the shader worker pool when
/// dispatched live so SPIRV-Cross, MSL compilation, and MTL4 PSO creation overlap
/// across workers without blocking the GPU thread.
class ComputePipeline : Common::NonCopyable, Common::NonMoveable {
public:
    /// @param worker_thread Pool the SPIRV-Cross + MSL + PSO compile runs on.
    ///                      nullptr builds synchronously (disk cache preload path).
    /// @param workgroup_size Declared threadgroup dimensions, from
    ///                       Shader::IR::Program (Shader::Info lacks it).
    explicit ComputePipeline(const Device& device, Scheduler& scheduler,
                             ShaderWorkerPool* worker_thread, const Shader::Info& info,
                             const std::array<u32, 3>& workgroup_size, std::vector<u32> spirv,
                             VideoCore::ShaderNotify* shader_notify = nullptr);
    ~ComputePipeline();

    /// Optimistic while an async build is in flight; becomes false only if the build
    /// finishes with an error. Callers must re-check Handle() after WaitBuilt().
    [[nodiscard]] bool IsValid() const noexcept {
        return !build_failed.load(std::memory_order::acquire);
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Blocks until the asynchronous build finishes. Call from the scheduler's
    /// worker thread, never from the thread recording dispatches.
    void WaitBuilt() const;

    /// Null until the build finishes, and stays null if it failed.
    [[nodiscard]] MTL::ComputePipelineState* Handle() const noexcept {
        return pipeline_state.get();
    }

    [[nodiscard]] const CompactStageInfo& GetInfo() const noexcept {
        return info;
    }

    /// Per-constant-buffer sizes, shaped for SetComputeUniformBufferState.
    [[nodiscard]] const VideoCommon::ComputeUniformBufferSizes& UniformBufferSizes()
        const noexcept {
        return uniform_buffer_sizes;
    }

    [[nodiscard]] MTL::Size GetThreadgroupSize() const noexcept {
        return threadgroup_size;
    }

    /// Largest total threadgroup this kernel can run at. Depends on register
    /// pressure, so it may be below the declared size. Valid after WaitBuilt().
    [[nodiscard]] u32 GetMaxTotalThreadsPerThreadgroup() const noexcept {
        return max_total_threads;
    }

    [[nodiscard]] u32 ExpectedArgumentSlots() const noexcept {
        return expected_argument_slots.load(std::memory_order::acquire);
    }

    [[nodiscard]] u32 ExpectedTableArgumentSlots() const noexcept {
        return expected_table_argument_slots.load(std::memory_order::acquire);
    }

private:
    void Build(std::vector<u32> spirv, const Shader::Info& full_info);
    void SignalBuilt();

    const Device& device;
    Scheduler& scheduler;
    CompactStageInfo info;
    std::atomic<VideoCore::ShaderNotify*> shader_notify{nullptr};

    Owned<MTL::ComputePipelineState> pipeline_state;

    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};

    MTL::Size threadgroup_size{1, 1, 1};
    u32 max_total_threads{1};
    std::atomic<u32> expected_argument_slots{0};
    std::atomic<u32> expected_table_argument_slots{0};

    mutable std::mutex build_mutex;
    mutable std::condition_variable build_condvar;
    std::atomic<bool> is_built{false};
    std::atomic<bool> build_failed{false};
};

} // namespace Metal
