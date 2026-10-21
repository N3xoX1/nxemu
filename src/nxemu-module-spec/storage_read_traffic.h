#pragma once

#include <cstdint>

// Native files under the configured NAND/SD mounts retain their emulated
// device identity. Files mounted directly as game content use a separate
// virtual-content device rather than their host PC's drive identity.
enum class StorageReadDomain : std::uint32_t
{
    Content,
    Nand,
    SdCard,
    Count
};

struct StorageReadTraffic
{
    std::uint64_t bytes[static_cast<std::uint32_t>(StorageReadDomain::Count)]{};
};
