// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include "yuzu_video_core/textures/decoders.h"
#include <algorithm>
#include <bitset>
#include <cstdlib>

namespace Tegra::Engines {

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}, upload_state{memory_manager, regs.upload} {
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
}

KeplerCompute::~KeplerCompute() = default;

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}

void KeplerCompute::ConsumeSinkImpl() {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void KeplerCompute::CallMethod(u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS,
               "Invalid KeplerCompute register, increase the size of the Regs structure");

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        upload_target = upload_state.ExecTargetAddress();
        upload_size = upload_state.GetUploadSize();
        uploaded_bytes = 0;
        upload_linear = regs.exec_upload.linear != 0 && regs.upload.line_count == 1;
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        RecordUploadSource(sizeof(u32));
        upload_state.ProcessData(method_argument, is_last_call && uploaded_bytes == upload_size);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (const auto & data : uploads)
        {
            for (u32 word = 0; word < 2; ++word)
            {
                const GPUVAddr target = launch_desc_loc +
                                        (LAUNCH_REG_INDEX(grid_dim_x) + word) * sizeof(u32);
                if (target < data.exec_address || target - data.exec_address + sizeof(u32) > data.copy_size)
                {
                    continue;
                }
                const GPUVAddr source = data.upload_address + target - data.exec_address;
                auto & indirect = word == 0 ? indirect_compute : indirect_compute_yz;
                // A later CPU upload to this field supersedes an earlier GPU source.
                indirect = data.was_dirty || memory_manager.IsMemoryDirty(source, sizeof(u32))
                               ? std::optional<GPUVAddr>{source}
                               : std::nullopt;
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        indirect_compute_yz = std::nullopt;
        upload_size = uploaded_bytes = 0;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                    u32 methods_pending) {
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
    {
        const bool is_continuation = uploaded_bytes != 0;
        RecordUploadSource(amount * sizeof(u32));
        if (is_continuation || uploaded_bytes < upload_size)
        {
            // A non-incrementing command can continue in another GP entry. The bulk upload
            // path expects the entire transfer, so accumulate partial payloads instead.
            for (u32 i = 0; i < amount; ++i)
            {
                upload_state.ProcessData(base_start[i],
                                         uploaded_bytes == upload_size && i + 1 == amount);
            }
        }
        else
        {
            upload_state.ProcessData(base_start, amount);
        }
        return;
    }
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void KeplerCompute::RecordUploadSource(u32 bytes)
{
    const u32 count = std::min(bytes, upload_size - uploaded_bytes);
    if (upload_linear && count != 0)
    {
        const UploadInfo info{.upload_address = current_dma_segment,
                              .exec_address = upload_target + uploaded_bytes,
                              .copy_size = count,
                              .was_dirty = current_dirty};
        if (!uploads.empty() && uploads.back().was_dirty == info.was_dirty &&
            uploads.back().upload_address + uploads.back().copy_size == info.upload_address &&
            uploads.back().exec_address + uploads.back().copy_size == info.exec_address) {
            uploads.back().copy_size += count;
        } else {
            uploads.push_back(info);
        }
    }
    uploaded_bytes += count;
    current_dirty = false;
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    static const bool diagnostic = [] {
        const char * value = std::getenv("NXEMU_GPU_BINDING_DIAGNOSTICS");
        return value && value[0] == '1';
    }();
    if (diagnostic && (indirect_compute || indirect_compute_yz))
    {
        LOG_INFO(HW_GPU,
                 "Compute launch: program=0x{:x}, qmd=0x{:x}, cpu_grid=({},{},{}), "
                 "source_x=0x{:x}, source_yz=0x{:x}",
                 launch_description.program_start, launch_desc_loc,
                 launch_description.grid_dim_x.Value(), launch_description.grid_dim_y.Value(),
                 launch_description.grid_dim_z.Value(), indirect_compute.value_or(0),
                 indirect_compute_yz.value_or(0));
    }
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

} // namespace Tegra::Engines
