// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>

#include "yuzu_common/common_types.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    u32 GetGuestSyncpointValue(u32 id) const {
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    struct RegisteredAction {
        explicit RegisteredAction(u32 expected_value_, u32 action_id_, std::function<void()>&& action_)
            : expected_value{expected_value_}, action_id{ action_id_}, action{std::move(action_)} {}
        u32 action_id;
        u32 expected_value;
        std::function<void()> action;
    };

    template <typename Func>
    uint32_t RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    uint32_t RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, uint32_t handle);

    void DeregisterHostAction(u32 syncpoint_id, uint32_t handle);

    void IncrementGuest(u32 syncpoint_id);

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return HasReached(syncpoints_guest[syncpoint_id].load(std::memory_order_acquire), expected_value);
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return HasReached(syncpoints_host[syncpoint_id].load(std::memory_order_acquire), expected_value);
    }

private:
    // Syncpoint fences use modulo-2^32 ordering within a half-counter range.
    static bool HasReached(u32 value, u32 threshold) {
        return static_cast<s32>(value - threshold) >= 0;
    }

    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   std::list<RegisteredAction>& action_storage);

    uint32_t RegisterAction(std::atomic<u32>& syncpoint,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(std::list<RegisteredAction>& action_storage, uint32_t handle);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value);

    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::atomic<uint32_t> next_action_id{ 1 };
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    std::mutex guard;
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
};

} // namespace Host1x

} // namespace Tegra
