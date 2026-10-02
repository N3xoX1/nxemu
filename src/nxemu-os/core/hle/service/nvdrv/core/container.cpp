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

Session::Session(SessionId id_, Kernel::KProcess * process_, Core::Asid asid_, IVideo& video_) :
    id{id_},
    process{process_},
    asid{asid_},
    has_preallocated_area{},
    mapper{},
    is_active{}, video{video_}
{
    process->Open();
}

Session::~Session() {
    // Nvmap allocations can outlive the last device FD, e.g. a retained
    // compositor framebuffer. Keep their mapper, ASID and process together.
    if (mapper) {
        const auto region_start = mapper->GetRegionStart();
        const auto region_size = mapper->GetRegionSize();
        mapper.reset();
        video.Host1xFree(region_start, region_size);
    }
    video.Host1xUnregisterProcess(asid.id);
    process->Close();
}

struct ContainerImpl
{
    explicit ContainerImpl(Container & core, IVideo & video_) :
        video{video_}, 
        file{core, video_}, 
        manager{video_}, 
        device_file_data{},
        zbc_state{}
    {
    }
    IVideo & video;
    NvMap file;
    SyncpointManager manager;
    Container::Host1xDeviceFileData device_file_data;
    Container::ZbcState zbc_state;
    std::deque<std::shared_ptr<Session>> sessions;
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
        if (!session || !session->is_active)
        {
            continue;
        }
        if (session->process == process)
        {
            session->ref_count++;
            return session->id;
        }
    }
    size_t new_id{};
    Core::Asid asid;
    asid.id = impl->video.Host1xRegisterProcess(&process->GetCoreMemory());
    if (!impl->id_pool.empty())
    {
        new_id = impl->id_pool.front();
        impl->id_pool.pop_front();
        impl->sessions[new_id] = std::make_shared<Session>(SessionId{new_id}, process, asid, impl->video);
    }
    else
    {
        new_id = impl->new_ids++;
        impl->sessions.emplace_back(std::make_shared<Session>(SessionId{new_id}, process, asid, impl->video));
    }
    auto & session = *impl->sessions[new_id];
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
    std::shared_ptr<Session> session;
    {
        std::scoped_lock lk(impl->session_guard);
        if (session_id.id >= impl->sessions.size() || !impl->sessions[session_id.id]) {
            return;
        }
        session = impl->sessions[session_id.id];
        if (--session->ref_count > 0) {
            return;
        }
        {
            std::scoped_lock session_lock(session->nvmap_mutex);
            session->is_active.store(false, std::memory_order_release);
        }
        impl->sessions[session_id.id].reset();
        impl->id_pool.emplace_front(session_id.id);
    }
    // Cleanup can wait on handle/page-table locks. Do not retain the container lock.
    impl->file.UnmapAllHandles(session);
}

std::shared_ptr<Session> Container::GetSessionReference(SessionId session_id) {
    std::scoped_lock lk{impl->session_guard};
    if (session_id.id >= impl->sessions.size()) return nullptr;
    const auto& session = impl->sessions[session_id.id];
    if (!session || !session->is_active.load(std::memory_order_acquire)) return nullptr;
    return session;
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

Container::ZbcState & Container::Zbc()
{
    return impl->zbc_state;
}

const Container::ZbcState & Container::Zbc() const
{
    return impl->zbc_state;
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
