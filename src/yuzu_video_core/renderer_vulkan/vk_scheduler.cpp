// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "yuzu_video_core/renderer_vulkan/vk_query_cache.h"

#include "yuzu_common/spin_lock.h"
#include "yuzu_common/thread.h"
#include "yuzu_video_core/renderer_vulkan/vk_command_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_gpu_timer.h"
#include "yuzu_video_core/renderer_vulkan/vk_master_semaphore.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_stall_profile.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "video_settings.h"
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
#include "yuzu_common/android/performance_hint.h"
#endif

namespace Vulkan {

namespace {
/// A worker that ran out of chunks spins this long before sleeping, but only while most recent
/// chunks came within it: then the spin costs less than the wake the GPU thread would pay.
constexpr auto WorkerSpinWindow = std::chrono::microseconds{50};
/// Chunks dispatched without urgency wake a sleeping worker once this many wait
constexpr u32 LazyWakeChunks = 2;
} // Anonymous namespace

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    frame_budget.reset();
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)},
      gpu_timer{std::make_unique<GpuTimer>(device, *master_semaphore)} {
    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                     std::optional<std::chrono::nanoseconds> frame_budget) {
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore, frame_budget);
    AllocateNewContext();
    return signal_value;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    const StallProfile profile{"finish", CurrentTick()};
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    const StallProfile profile{"worker_wait", CurrentTick()};
    DispatchWork();
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_GPU_THREAD_ADPF)
    const Common::Android::PerformanceHintWait hint_wait;
#endif

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        ++queue_waiters;
        event_cv.wait(ql, [this] { return work_queue.empty(); });
        --queue_waiters;
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork(bool urgent) {
    u32 queued;
    if (chunk->Empty()) {
        if (!urgent) {
            return;
        }
        // Chunks dispatched without urgency may still wait for a sleeping worker
        queued = pending_chunks.load(std::memory_order_seq_cst);
    } else {
        std::scoped_lock ql{queue_mutex};
        work_queue.push(std::move(chunk));
        queued = pending_chunks.fetch_add(1, std::memory_order_seq_cst) + 1;
    }
    // A busy or spinning worker sees the count. Waking a sleeping one costs this thread a
    // system call, so it is woken only when needed.
    if (queued != 0 && (urgent || queued >= LazyWakeChunks) &&
        worker_sleeping.load(std::memory_order_seq_cst)) {
        pending_chunks.notify_one();
    }
    if (!chunk) {
        AcquireNewChunk();
    }
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer, u32 color_scratch_mask) {
    color_scratch_mask &= framebuffer->DuplicateColorAttachmentMask();
    const VkRenderPass renderpass = framebuffer->RenderPassVariant(color_scratch_mask);
    const VkFramebuffer framebuffer_handle = framebuffer->HandleVariant(color_scratch_mask);
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    EndRenderPass();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    Record([renderpass, framebuffer_handle, render_area](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = 0,
            .pClearValues = nullptr,
        };
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images(color_scratch_mask);
    renderpass_image_ranges = framebuffer->ImageRanges(color_scratch_mask);
    if (query_cache) {
        query_cache->NotifyRenderPass(true);
    }
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        return false;
    }
    state.graphics_pipeline = pipeline;
    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
#ifdef __APPLE__
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
#endif

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
    Common::Android::PerformanceHintSession performance_hint;
#endif

    // The worker sleeps on pending_chunks: stopping must wake it
    const std::stop_callback wake_on_stop{stop_token, [this] {
        pending_chunks.fetch_add(1, std::memory_order_seq_cst);
        pending_chunks.notify_all();
    }};
    u32 quick_idles = 0;

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
        std::optional<std::chrono::nanoseconds> frame_budget;
#endif

        if (pending_chunks.load(std::memory_order_acquire) == 0) {
            WaitForChunks(stop_token, quick_idles);
        }

        {
            std::unique_lock lk{queue_mutex};

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }
            if (work_queue.empty()) {
                continue;
            }
            work = std::move(work_queue.front());
            work_queue.pop();
            pending_chunks.fetch_sub(1, std::memory_order_relaxed);
            if (work_queue.empty() && queue_waiters != 0) {
                event_cv.notify_all();
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            std::exchange(lk, std::unique_lock{execution_mutex});

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
            frame_budget = work->frame_budget;
            performance_hint.BeginWork();
#endif
            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            {
                const StallProfile profile{"worker_commands", CurrentTick()};
                work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);
            }

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
            performance_hint.EndWork();
#endif
        }

#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
        if (frame_budget) {
            performance_hint.ReportFrame(*frame_budget);
        }
#endif

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::WaitForChunks(std::stop_token stop_token, u32& quick_idles) {
    const auto idle_start = std::chrono::steady_clock::now();
    bool found = false;
    // Spin when at least half of the last 8 idle periods were shorter than the window
    if (WorkerSpinWindow.count() != 0 && std::popcount(quick_idles & 0xFF) >= 4) {
        const auto spin_end = idle_start + WorkerSpinWindow;
        u32 iterations = 0;
        while (!(found = pending_chunks.load(std::memory_order_acquire) != 0) &&
               !stop_token.stop_requested()) {
            // pause on x86, yield on ARM64: no system call, the worker keeps its priority
            Common::ThreadPause();
            if ((++iterations & 63) == 0 && std::chrono::steady_clock::now() >= spin_end) {
                break;
            }
        }
    }
    if (!found) {
        // DispatchWork reads the flag after counting its chunk, this reads the count after
        // setting the flag: one of the two sees the other
        worker_sleeping.store(true, std::memory_order_seq_cst);
        while (pending_chunks.load(std::memory_order_seq_cst) == 0 &&
               !stop_token.stop_requested()) {
            pending_chunks.wait(0, std::memory_order_seq_cst);
        }
        worker_sleeping.store(false, std::memory_order_relaxed);
    }
    if (WorkerSpinWindow.count() != 0) {
        const bool quick = std::chrono::steady_clock::now() - idle_start <= WorkerSpinWindow;
        quick_idles = (quick_idles << 1) | (quick ? 1U : 0U);
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    gpu_timer->Begin(current_upload_cmdbuf);
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                               std::optional<std::chrono::nanoseconds> frame_budget) {
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        gpu_timer->End(cmdbuf, signal_value);
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{device.QueueMutex()};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            break;
        case VK_ERROR_DEVICE_LOST:
            master_semaphore->MarkDeviceLost();
            break;
        default:
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    chunk->frame_budget = frame_budget;
    DispatchWork();
    return signal_value;
}

void Scheduler::MarkBufferWrite(VkBuffer buffer) {
    // Past this many buffers, searching them costs more than what reordering commands saves
    static constexpr size_t MAX_WRITTEN_BUFFERS = 64;
    if (buffer == VK_NULL_HANDLE || IsBufferWritten(buffer)) {
        return;
    }
    if (written_buffers.size() == MAX_WRITTEN_BUFFERS) {
        all_buffers_written = true;
        return;
    }
    written_buffers.push_back(buffer);
}

bool Scheduler::IsBufferWritten(VkBuffer buffer) const noexcept {
    return all_buffers_written ||
           std::ranges::find(written_buffers, buffer) != written_buffers.end();
}

void Scheduler::AllocateNewContext() {
    ++record_epoch;
    written_buffers.clear();
    all_buffers_written = false;
    // Enable counters once again. These are disabled when a command buffer is finished.
    if (query_cache) {
#if ANDROID
        if (Settings::IsGPULevelHigh()) {
            // This is problematic on Android, disable on GPU Normal.
            query_cache->NotifySegment(true);
        }
#else
        query_cache->NotifySegment(true);
#endif
    }
}

void Scheduler::InvalidateState() {
    ++record_epoch;
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
#if ANDROID
    if (Settings::IsGPULevelHigh()) {
        // This is problematic on Android, disable on GPU Normal.
        // query_cache->DisableStreams();
    }
#else
    // query_cache->DisableStreams();
#endif
    if (query_cache) {
        query_cache->NotifySegment(false);
    }
    EndRenderPass();
}

void Scheduler::ComputeMemoryBarrier(vk::CommandBuffer cmdbuf) {
    static constexpr VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    // Submission order alone does not make storage buffer/image writes visible. Include
    // all consumers: compute, graphics, indirect commands and transfers to sparse aliases.
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
}

void Scheduler::EndRenderPass() {
    if (!state.renderpass) {
        return;
    }
    if (query_cache) {
        query_cache->NotifyRenderPass(false);
    }
    Record([num_images = num_renderpass_images, images = renderpass_images,
            ranges = renderpass_image_ranges,
            counters_written = std::exchange(transform_feedback_counters_written, false)](
               vk::CommandBuffer cmdbuf) {
        std::array<VkImageMemoryBarrier, 9> barriers;
        for (size_t i = 0; i < num_images; ++i) {
            barriers[i] = VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[i],
                .subresourceRange = ranges[i],
            };
        }
        cmdbuf.EndRenderPass();
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, nullptr,
                               vk::Span(barriers.data(), num_images));
        // A barrier recorded in a render pass orders only commands of that pass
        if (counters_written) {
            static constexpr VkMemoryBarrier COUNTER_BARRIER{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                .dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                   VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, 0,
                                   COUNTER_BARRIER);
        }
    });
    state.renderpass = nullptr;
    num_renderpass_images = 0;
    ++render_pass_end_tick;
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}
} // namespace Vulkan
