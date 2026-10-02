// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "yuzu_common/nvdata.h"
#include <nxemu-module-spec/video.h>

namespace Kernel {
class KProcess;
}

namespace Tegra::Host1x {
class Host1x;
} // namespace Tegra::Host1x

namespace Service::Nvidia::NvCore {

class HeapMapper;
class NvMap;
class SyncpointManager;

struct ContainerImpl;

struct SessionId {
    size_t id;
};

struct Session {
    Session(SessionId id_, Kernel::KProcess* process_, Core::Asid asid_, IVideo& video_);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    SessionId id;
    Kernel::KProcess* process;
    Core::Asid asid;
    bool has_preallocated_area{};
    std::unique_ptr<HeapMapper> mapper{};
    std::atomic_bool is_active{};
    std::mutex nvmap_mutex; //!< Serializes guest reference creation with session closure.
    s32 ref_count{};
    IVideo& video;
};

class Container {
public:
    explicit Container(IVideo & video);
    ~Container();

    SessionId OpenSession(Kernel::KProcess* process);
    void CloseSession(SessionId id);

    std::shared_ptr<Session> GetSessionReference(SessionId id);

    NvMap& GetNvMapFile();

    const NvMap& GetNvMapFile() const;

    SyncpointManager& GetSyncpointManager();

    const SyncpointManager& GetSyncpointManager() const;

    struct Host1xDeviceFileData {
        std::unordered_map<DeviceFD, u32> fd_to_id{};
        std::deque<u32> syncpts_accumulated{};
        u32 nvdec_next_id{};
        u32 vic_next_id{};
    };

    struct ZbcColorEntry {
        std::array<u32, 4> color_ds{};
        std::array<u32, 4> color_l2{};
        u32 format{};
        u32 ref_cnt{};
    };

    struct ZbcDepthEntry {
        u32 depth{};
        u32 format{};
        u32 ref_cnt{};
    };

    struct ZbcState {
        static constexpr u32 TableSize = 15;

        std::mutex mutex;
        std::array<ZbcColorEntry, TableSize> color_table{};
        std::array<ZbcDepthEntry, TableSize> depth_table{};
        u32 max_used_color_index{};
        u32 max_used_depth_index{};
    };

    Host1xDeviceFileData& Host1xDeviceFile();

    const Host1xDeviceFileData& Host1xDeviceFile() const;

    ZbcState& Zbc();
    const ZbcState& Zbc() const;

private:
    std::unique_ptr<ContainerImpl> impl;
};

} // namespace Service::Nvidia::NvCore
