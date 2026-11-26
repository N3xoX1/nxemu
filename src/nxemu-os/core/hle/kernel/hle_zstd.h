#pragma once

#include <cstdint>
#include <span>

namespace Core
{
class System;
}

namespace Kernel::HleZstd
{
// Handle synthetic HLE Zstd SVC calls (HleZstd::kSvc*).
// Returns true if `imm` is an HLE Zstd SVC (with args[0] set to decoded size
// or HleZstd::kFallbackReturn). Returns false for all other SVC immediates.
bool HandleSvc(Core::System & system, uint32_t imm, std::span<uint64_t, 8> args);

// Release cached host ZSTD_DDict instances (call on process init/shutdown).
void Reset();
} // namespace Kernel::HleZstd
