// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_master_semaphore.h"
#include "yuzu_video_core/renderer_metal/mtl_index_pass.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"

namespace Metal {
namespace {
constexpr u64 WAIT_SLICE_MS = 1000;

// Timed-out slices in a row before we complain. We keep waiting either way.
constexpr u32 WAIT_SLICES_BEFORE_WARNING = 5;

// MTL4CommandQueueErrorDomain code for "Caused GPU Address Fault Error". Comes in
// as the underlying error of a generic code 1, so the top-level code tells us nothing.
constexpr NS::Integer FAULT_ERROR_CODE = 2;

// MTL4CommandQueueErrorDomain code for "Ignored (for causing prior/excessive GPU
// errors)". Once the queue starts saying this it will not run anything else.
constexpr NS::Integer IGNORED_ERROR_CODE = 5;

const NS::Error* FirstUnderlyingError(const NS::Error* error) {
    NS::Error* const mutable_error = const_cast<NS::Error*>(error);
    NS::Dictionary* const info = mutable_error->userInfo();
    if (info == nullptr) {
        return nullptr;
    }
    return static_cast<const NS::Error*>(
        info->object(NS::String::string("NSUnderlyingError", NS::UTF8StringEncoding)));
}

std::string PixelFormatName(u32 format) {
    switch (static_cast<MTL::PixelFormat>(format)) {
    case MTL::PixelFormatInvalid:
        return "none";
    case MTL::PixelFormatBGRA8Unorm:
        return "BGRA8";
    case MTL::PixelFormatBGRA8Unorm_sRGB:
        return "BGRA8sRGB";
    case MTL::PixelFormatRGBA8Unorm:
        return "RGBA8";
    case MTL::PixelFormatRGBA8Unorm_sRGB:
        return "RGBA8sRGB";
    case MTL::PixelFormatRGBA16Float:
        return "RGBA16F";
    case MTL::PixelFormatRGBA32Float:
        return "RGBA32F";
    case MTL::PixelFormatRG11B10Float:
        return "RG11B10F";
    case MTL::PixelFormatRGB10A2Unorm:
        return "RGB10A2";
    case MTL::PixelFormatR8Unorm:
        return "R8";
    case MTL::PixelFormatRG8Unorm:
        return "RG8";
    case MTL::PixelFormatR16Float:
        return "R16F";
    case MTL::PixelFormatRG16Float:
        return "RG16F";
    case MTL::PixelFormatR32Float:
        return "R32F";
    case MTL::PixelFormatDepth16Unorm:
        return "D16";
    case MTL::PixelFormatDepth32Float:
        return "D32F";
    case MTL::PixelFormatDepth32Float_Stencil8:
        return "D32FS8";
    case MTL::PixelFormatDepth24Unorm_Stencil8:
        return "D24S8";
    case MTL::PixelFormatStencil8:
        return "S8";
    default:
        return fmt::format("fmt{}", format);
    }
}

} // Anonymous namespace

std::string DescribeTarget(const SubmissionContents& contents) {
    if (contents.pass_width == 0) {
        return "no render target";
    }
    return fmt::format("{}x{}{} {}+{}", contents.pass_width, contents.pass_height,
                       contents.pass_samples > 1 ? fmt::format("x{}msaa", contents.pass_samples)
                                                 : std::string{},
                       PixelFormatName(contents.color_format),
                       PixelFormatName(contents.depth_format));
}

MasterSemaphore::MasterSemaphore(const Device& device)
    : event{Adopt(device.GetDevice()->newSharedEvent())} {
    if (!event) {
        throw Exception("Failed to create the master MTLSharedEvent");
    }
    event->setLabel(MTLSTR("nxemu master semaphore"));
    // Tick 0 means "nothing submitted yet" and must read as already complete.
    event->setSignaledValue(0);
}

MasterSemaphore::~MasterSemaphore() {
    // Wait for any in-flight MTL4 commit feedback callbacks to finish before
    // tearing down `this` (the GPU shared event can signal before Metal's host
    // dispatch queue invokes the feedback block).
    std::unique_lock lock{callback_mutex};
    shutting_down.store(true, std::memory_order_seq_cst);
    callback_cv.wait_for(lock, std::chrono::seconds{5}, [this] {
        return pending_callbacks.load(std::memory_order_seq_cst) == 0;
    });
}

void MasterSemaphore::AdvanceGpuTick(u64 value) noexcept {
    u64 known = gpu_tick.load(std::memory_order_acquire);
    while (known < value &&
           !gpu_tick.compare_exchange_weak(known, value, std::memory_order_release,
                                           std::memory_order_acquire)) {
        // compare_exchange_weak refreshed `known`, loop back and re-check.
    }
}

void MasterSemaphore::Refresh() {
    AdvanceGpuTick(event->signaledValue());
}

void MasterSemaphore::Wait(u64 tick) {
    if (IsFree(tick)) {
        return;
    }

    // Sliced only so a hung GPU is diagnosable. The wait itself never times out.
    for (u32 slice = 0;; ++slice) {
        if (event->waitUntilSignaledValue(tick, WAIT_SLICE_MS)) {
            break;
        }

        Refresh();
        if (IsFree(tick)) {
            break;
        }

        if (slice == WAIT_SLICES_BEFORE_WARNING) {
            LOG_WARNING(Render_Metal,
                        "Still waiting on GPU tick {} after {} ms (GPU is at {}); "
                        "the device may be hung",
                        tick, WAIT_SLICE_MS * (slice + 1), KnownGpuTick());
            // Usually a drawable we waited on and never presented, which wedges
            // the queue with no error at all.
            if (on_stall) {
                on_stall();
            }
        }
    }

    AdvanceGpuTick(tick);
}

void MasterSemaphore::SignalAndCommitMTL4(MTL4::CommandQueue* queue,
                                          MTL4::CommandBuffer* command_buffer, u64 host_tick,
                                          u64 carried_tick, SubmissionContents contents,
                                          MTL4::CommandBuffer* upload_buffer) {
    host_tick = std::max(host_tick, carried_tick);

    // Metal 4 separates "stop encoding into this" from "hand it to the GPU".
    command_buffer->endCommandBuffer();

    // Commit-to-start is queue wait (drawable gate), start-to-end is the work.
    const double commit_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    pending_callbacks.fetch_add(1, std::memory_order_relaxed);

    MTL4::CommitOptions* options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler([this, host_tick, commit_time, contents](MTL4::CommitFeedback* feedback) {
        if (feedback != nullptr) {
            const double started = feedback->GPUStartTime();
            const double ended = feedback->GPUEndTime();

            if (NS::Error* const error = feedback->error()) {
                // A run of these means the queue stopped executing for good.
                if (const NS::Error* const cause = FirstUnderlyingError(error);
                    (cause != nullptr && cause->code() == IGNORED_ERROR_CODE) ||
                    error->code() == IGNORED_ERROR_CODE) {
                    ignored_streak.fetch_add(1, std::memory_order_relaxed);
                }
                LOG_ERROR(Render_Metal,
                          "Metal 4 commit for tick {} failed after queued {:.2f} ms, ran "
                          "{:.2f} ms (start {:.4f} end {:.4f}): {}",
                          host_tick, (started - commit_time) * 1000.0,
                          (ended - started) * 1000.0, started, ended, DescribeError(error));
                // Counted so the rasterizer submits smaller batches, see
                // RasterizerMetal::FlushWork. Only real timeouts though, Legends
                // Z-A fails with page faults instead, a code 1 wrapping an
                // underlying code 2. That one is residency, see SweepKeepAlive.
                if (error->code() == MTL4::CommandQueueErrorTimeout) {
                    timeouts.fetch_add(1, std::memory_order_relaxed);
                }
                if (const NS::Error* const cause = FirstUnderlyingError(error);
                    cause != nullptr && cause->code() == FAULT_ERROR_CODE) {
                    const u32 n = faults.fetch_add(1, std::memory_order_relaxed) + 1;
                    // Which encoders the submission opened says what to suspect.
                    LOG_ERROR(Render_Metal,
                              "GPU page fault ({} so far) in a submission holding {} render / {} "
                              "compute / {} copy encoders into {}: a shader addressed memory that "
                              "is not in the residency set",
                              n, contents.render_encoders, contents.compute_encoders,
                              contents.blit_encoders, DescribeTarget(contents));
                }
            } else {
                // Ran to completion, so the queue is alive whatever it did before.
                ignored_streak.store(0, std::memory_order_relaxed);
            }
        }
        AdvanceGpuTick(host_tick);
        if (pending_callbacks.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
            shutting_down.load(std::memory_order_seq_cst)) {
            std::scoped_lock lock{callback_mutex};
            callback_cv.notify_all();
        }
    });

    const MTL4::CommandBuffer* buffers[2];
    NS::UInteger count = 0;
    if (upload_buffer != nullptr) {
        buffers[count++] = upload_buffer;
    }
    buffers[count++] = command_buffer;
    queue->commit(buffers, count, options);
    options->release();

    // Has to follow the commit: queue operations run in submission order.
    queue->signalEvent(event.get(), host_tick);
}

} // namespace Metal
