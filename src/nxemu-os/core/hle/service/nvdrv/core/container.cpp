// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <deque>
#include <mutex>

#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/heap_mapper.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/memory.h"

#include <nxemu-module-spec/video.h>

namespace Service::Nvidia::NvCore
{

Session::Session(SessionId id_, Kernel::KProcess * process_, Core::Asid asid_) :
    id{id_},
    process{process_},
    asid{asid_},
    has_preallocated_area{},
    mapper{},
    is_active{}
{
}

Session::~Session() = default;

struct ContainerImpl
{
    explicit ContainerImpl(Container & core, IVideo & video_) :
        video{video_}, 
        file{core, video_}, 
        manager{video_}, 
        device_file_data{}
    {
    }
    IVideo & video;
    NvMap file;
    SyncpointManager manager;
    Container::Host1xDeviceFileData device_file_data;
    std::deque<Session> sessions;
    size_t new_ids{};
    std::deque<size_t> id_pool;
    std::mutex session_guard;
};

Container::Container(IVideo & video)
{
    impl = std::make_unique<ContainerImpl>(*this, video);
}

Container::~Container() = default;

SessionId Container::OpenSession(Kernel::KProcess * process)
{
    using namespace Common::Literals;

    std::scoped_lock lk(impl->session_guard);
    for (auto & session : impl->sessions)
    {
        if (!session.is_active)
        {
            continue;
        }
        if (session.process == process)
        {
            session.ref_count++;
            return session.id;
        }
    }
    size_t new_id{};
    Core::Asid asid;
    asid.id = impl->video.Host1xRegisterProcess(&process->GetCoreMemory());
    if (!impl->id_pool.empty())
    {
        new_id = impl->id_pool.front();
        impl->id_pool.pop_front();
        impl->sessions[new_id] = Session{SessionId{new_id}, process, asid};
    }
    else
    {
        new_id = impl->new_ids++;
        impl->sessions.emplace_back(SessionId{new_id}, process, asid);
    }
    auto & session = impl->sessions[new_id];
    session.is_active = true;
    session.ref_count = 1;
    // Optimization
    if (process->IsApplication())
    {
        auto & page_table = process->GetKPageTable().GetBasePageTable();
        auto heap_start = page_table.GetHeapRegionStart();

        Kernel::KProcessAddress cur_addr = heap_start;
        size_t region_size = 0;
        VAddr region_start = 0;
        while (true)
        {
            Kernel::KMemoryInfo mem_info{};
            Kernel::Svc::PageInfo page_info{};
            R_ASSERT(page_table.QueryInfo(std::addressof(mem_info), std::addressof(page_info),
                                          cur_addr));
            auto svc_mem_info = mem_info.GetSvcMemoryInfo();

            // Check if this memory block is heap.
            if (svc_mem_info.state == Kernel::Svc::MemoryState::Normal)
            {
                if (region_start + region_size == svc_mem_info.base_address)
                {
                    region_size += svc_mem_info.size;
                }
                else if (svc_mem_info.size > region_size)
                {
                    region_size = svc_mem_info.size;
                    region_start = svc_mem_info.base_address;
                }
            }

            // Check if we're done.
            const uintptr_t next_address = svc_mem_info.base_address + svc_mem_info.size;
            if (next_address <= cur_addr.GetValue())
            {
                break;
            }

            cur_addr = next_address;
        }
        session.has_preallocated_area = false;
        auto start_region = region_size >= 32_MiB ? impl->video.Host1xMemoryAllocate(region_size) : 0;
        if (start_region != 0)
        {
            session.mapper = std::make_unique<HeapMapper>(region_start, start_region, region_size, asid, impl->video);
            impl->video.Host1xMemoryTrackContinuity(start_region, region_start, region_size, asid.id);
            session.has_preallocated_area = true;
            LOG_DEBUG(Debug, "Preallocation created!");
        }
    }
    return SessionId{new_id};
}

void Container::CloseSession(SessionId session_id)
{
    std::scoped_lock lk(impl->session_guard);
    auto & session = impl->sessions[session_id.id];
    if (--session.ref_count > 0)
    {
        return;
    }
    impl->file.UnmapAllHandles(session_id);
    if (session.has_preallocated_area)
    {
        const DAddr region_start = session.mapper->GetRegionStart();
        const size_t region_size = session.mapper->GetRegionSize();
        session.mapper.reset();
        impl->video.Host1xFree(region_start, region_size);
        session.has_preallocated_area = false;
    }
    session.is_active = false;
    impl->video.Host1xUnregisterProcess(impl->sessions[session_id.id].asid.id);
    impl->id_pool.emplace_front(session_id.id);
}

Session * Container::GetSession(SessionId session_id)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    return &impl->sessions[session_id.id];
}

NvMap & Container::GetNvMapFile()
{
    return impl->file;
}

const NvMap & Container::GetNvMapFile() const
{
    return impl->file;
}

Container::Host1xDeviceFileData & Container::Host1xDeviceFile()
{
    return impl->device_file_data;
}

const Container::Host1xDeviceFileData & Container::Host1xDeviceFile() const
{
    return impl->device_file_data;
}

SyncpointManager & Container::GetSyncpointManager()
{
    return impl->manager;
}

const SyncpointManager & Container::GetSyncpointManager() const
{
    return impl->manager;
}

} // namespace Service::Nvidia::NvCore
