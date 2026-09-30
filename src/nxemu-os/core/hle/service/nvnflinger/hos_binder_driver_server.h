// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "yuzu_common/common_types.h"
#include "core/hle/service/nvnflinger/binder.h"

namespace Core {
class System;
}

namespace Service::Nvnflinger {

class HosBinderDriverServer final {
public:
    explicit HosBinderDriverServer();
    ~HosBinderDriverServer();

    s32 RegisterBinder(std::shared_ptr<android::IBinder>&& binder);
    void UnregisterBinder(s32 binder_id);

    std::shared_ptr<android::IBinder> TryGetBinder(s32 id) const;

    void AdjustRefcount(s32 binder_id, s32 delta, bool is_weak);

private:
    struct RefCounts {
        s32 strong{1};
        s32 weak{0};
    };

    std::unordered_map<s32, std::shared_ptr<android::IBinder>> binders;
    std::unordered_map<s32, RefCounts> refcounts;
    mutable std::mutex lock;
    s32 last_id{};
};

} // namespace Service::Nvnflinger
