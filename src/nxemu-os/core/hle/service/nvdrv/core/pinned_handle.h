// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/nvdrv/core/nvmap.h"

namespace Service::Nvidia::NvCore
{

// A mapping is owned independently of the guest handle. Copies of this lease
// can outlive UnmapBuffer while previously submitted commands still use it.
class PinnedHandle final
{
public:
    static std::shared_ptr<PinnedHandle> Create(NvMap & nvmap, NvMap::Handle::Id id)
    {
        const auto handle = nvmap.GetHandle(id);
        if (!handle)
        {
            return {};
        }
        // Allocate before acquiring the internal reference, so allocation failure
        // cannot leak a reference to guest memory.
        auto lease = std::shared_ptr<PinnedHandle>(new PinnedHandle(nvmap, id));
        if (handle->Duplicate(true) != NvResult::Success)
        {
            return {};
        }
        lease->owns_reference = true;
        lease->address = nvmap.PinHandle(id, true);
        if (!lease->address)
        {
            return {};
        }
        return lease;
    }

    ~PinnedHandle()
    {
        if (address)
        {
            nvmap.UnpinHandle(id);
        }
        if (owns_reference)
        {
            nvmap.FreeHandle(id, true);
        }
    }

    PinnedHandle(const PinnedHandle &) = delete;
    PinnedHandle & operator=(const PinnedHandle &) = delete;

    NvMap::Handle::Id Id() const
    {
        return id;
    }
    DAddr Address() const
    {
        return address;
    }

private:
    PinnedHandle(NvMap & nvmap_, NvMap::Handle::Id id_) :
        nvmap{nvmap_}, id{id_}
    {
    }

    NvMap & nvmap;
    NvMap::Handle::Id id;
    DAddr address{};
    bool owns_reference{};
};

} // namespace Service::Nvidia::NvCore
