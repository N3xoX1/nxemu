// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace Core::Timing
{

// Minimal transfer-time model for backing reads. HS400 is the fastest eMMC
// link: 200 MHz, eight data lines, two transfers per clock. This is a bus
// ceiling, not a measured flash throughput or an estimate of FS cache latency.
class StorageReadBus
{
public:
    enum class Profile
    {
        EmmcHs400,
        SdSdr104
    };
    static constexpr std::uint64_t ClockHz = 200'000'000;
    static constexpr std::uint64_t DataLines = 8;
    static constexpr std::uint64_t TransfersPerClock = 2;
    static constexpr std::uint64_t BytesPerSecond = ClockHz * DataLines * TransfersPerClock / 8;
    static constexpr std::uint64_t Limit = std::numeric_limits<std::int64_t>::max();

    explicit StorageReadBus(Profile profile_ = Profile::EmmcHs400) :
        profile{profile_}
    {
    }

    static constexpr std::uint64_t Rate(Profile profile)
    {
        // SDR104: 208 MHz, four lines, one transfer per clock.
        return profile == Profile::SdSdr104 ? 208'000'000ULL * 4 / 8 : BytesPerSecond;
    }

    static constexpr std::uint64_t TransferNanoseconds(std::uint64_t bytes, Profile profile = Profile::EmmcHs400)
    {
        const auto rate = Rate(profile);
        constexpr std::uint64_t ns_per_second = 1'000'000'000;
        const auto seconds = bytes / rate;
        if (seconds > Limit / ns_per_second) return Limit;
        const auto whole = seconds * ns_per_second;
        const auto tail = ((bytes % rate) * ns_per_second + rate - 1) / rate;
        return whole + std::min(tail, Limit - whole);
    }

    std::uint64_t Reserve(std::uint64_t now, std::uint64_t bytes)
    {
        now = std::min(now, Limit);
        // A cache hit need not wait behind an unrelated pending transfer.
        if (!bytes) return now;
        std::scoped_lock lock{mutex};
        const auto start = std::max(now, available);
        available = start + std::min(TransferNanoseconds(bytes, profile), Limit - start);
        return available;
    }

    void Reset()
    {
        std::scoped_lock lock{mutex};
        available = 0;
    }

private:
    const Profile profile;
    std::mutex mutex;
    std::uint64_t available{};
};

} // namespace Core::Timing
