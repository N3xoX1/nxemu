// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_compute_pipeline.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/shader_notify.h"

namespace Metal {

ComputePipeline::ComputePipeline(const Device& device_, Scheduler& scheduler_,
                                 ShaderWorkerPool* worker_thread, const Shader::Info& info_,
                                 const std::array<u32, 3>& workgroup_size, std::vector<u32> spirv,
                                 VideoCore::ShaderNotify* shader_notify_)
    : device{device_}, scheduler{scheduler_}, info{info_}, shader_notify{shader_notify_},
      threadgroup_size{MTL::Size::Make(workgroup_size[0], workgroup_size[1], workgroup_size[2])} {
    const size_t num_cbufs =
        std::min(info.constant_buffer_used_sizes.size(), uniform_buffer_sizes.size());
    std::copy_n(info.constant_buffer_used_sizes.begin(), num_cbufs, uniform_buffer_sizes.begin());

    if (shader_notify_ != nullptr) {
        shader_notify_->MarkShaderBuilding();
    }

    if (worker_thread == nullptr) {
        Build(std::move(spirv));
        return;
    }
    // Compute dispatches cannot be skipped without corrupting GPU state, so prioritize
    // them ahead of speculative async graphics compiles.
    worker_thread->QueueWork(
        [this, code = std::move(spirv)]() mutable { Build(std::move(code)); },
        /*high_priority=*/true);
}

ComputePipeline::~ComputePipeline() {
    WaitBuilt();
    // Ensure SignalBuilt() has exited its critical section before destroying build_mutex.
    std::scoped_lock lock{build_mutex};
}

void ComputePipeline::SignalBuilt() {
    if (VideoCore::ShaderNotify* const notify = shader_notify.exchange(nullptr)) {
        notify->MarkShaderComplete();
    }
    std::scoped_lock lock{build_mutex};
    is_built.store(true, std::memory_order::release);
    build_condvar.notify_all();
}

void ComputePipeline::WaitBuilt() const {
    if (is_built.load(std::memory_order::acquire)) {
        return;
    }
    std::unique_lock lock{build_mutex};
    build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::acquire); });
}

void ComputePipeline::Build(std::vector<u32> spirv) {
    ScopedAutoreleasePool pool;

    const auto fail = [this] {
        build_failed.store(true, std::memory_order::release);
        SignalBuilt();
    };

    MslShader shader;
    try {
        const BindingMap binding_map{};
        shader = RunOnLargeStack([&]() {
            return TranslateSpirvToMsl(device, spirv, Shader::Stage::Compute, info, binding_map,
                                       /*rasterizes_points=*/false);
        });
        expected_argument_slots.store(shader.argument_buffer_slots, std::memory_order::release);
        expected_table_argument_slots.store(shader.table_argument_buffer_slots,
                                            std::memory_order::release);
    } catch (const std::exception& error) {
        LOG_ERROR(Render_Metal, "Failed to translate compute shader to MSL: {}", error.what());
        fail();
        return;
    }

    Owned<MTL::Library> library = BuildLibrary(device, shader);
    if (!library) {
        fail();
        return;
    }

    NS::Error* error = nullptr;
    {
        // Metal 4 takes a library plus entry point, not an MTL::Function.
        MTL4::ComputePipelineDescriptor* descriptor =
            MTL4::ComputePipelineDescriptor::alloc()->init();
        descriptor->setComputeFunctionDescriptor(
            MakeFunctionDescriptor(library.get(), shader.entry_point));
        descriptor->setLabel(MTLSTR("eden compute"));
        pipeline_state = device.NewComputePipelineState(descriptor, &error);
        descriptor->release();
    }
    if (!pipeline_state) {
        LOG_ERROR(Render_Metal, "Failed to create a compute pipeline state: {}",
                  DescribeError(error));
        fail();
        return;
    }

    // Dispatching above this is a hard error, and it can be below the declared
    // size for register-heavy kernels, so ask the pipeline.
    max_total_threads = static_cast<u32>(pipeline_state->maxTotalThreadsPerThreadgroup());

    const u32 declared_total =
        static_cast<u32>(threadgroup_size.width * threadgroup_size.height * threadgroup_size.depth);
    if (declared_total > max_total_threads) {
        LOG_WARNING(Render_Metal,
                    "Compute kernel declares {} threads per group but the device allows {}; "
                    "dispatches will be clamped and results may be wrong",
                    declared_total, max_total_threads);
    }

    SignalBuilt();
}

} // namespace Metal
