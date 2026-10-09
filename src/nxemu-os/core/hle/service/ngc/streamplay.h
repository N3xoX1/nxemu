// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace Core { class System; }
namespace Service { class ServerManager; }

namespace Service::NGC {

// SF/CMIF NotSupported (module 10, description 1), not HIPC (module 11).
inline constexpr uint32_t StreamPlayNotSupported = (1u << 9) | 10u;

struct StreamPlayCommand {
    uint32_t id;
    const char* name;
};

inline constexpr std::array UserShimCommands{
    StreamPlayCommand{450, "InitializeForSaveData"},
    StreamPlayCommand{451, "FinalizeForSaveData"},
    StreamPlayCommand{452, "OpenSaveData"},
    StreamPlayCommand{453, "CloseSaveData"},
    StreamPlayCommand{454, "ReadSaveSlot"},
    StreamPlayCommand{455, "WriteSaveSlot"},
    StreamPlayCommand{456, "FlushSaveSlot"},
    StreamPlayCommand{457, "CommitSaveData"},
};

inline constexpr std::array SystemShimCommands{
    StreamPlayCommand{106, "Cmd106"}, StreamPlayCommand{107, "Cmd107"},
    StreamPlayCommand{108, "Cmd108"}, StreamPlayCommand{207, "Cmd207"},
    StreamPlayCommand{208, "Cmd208"}, StreamPlayCommand{209, "Cmd209"},
    StreamPlayCommand{210, "Cmd210"}, StreamPlayCommand{211, "Cmd211"},
    StreamPlayCommand{212, "Cmd212"},
};

inline bool IsStreamPlayFirmwareSupported(std::string_view version) {
    if (version.empty()) {
        return false;
    }
    std::array<uint32_t, 3> components{};
    const char* cursor = version.data();
    const char* end = cursor + version.size();
    for (size_t i = 0; i < components.size(); ++i) {
        if (cursor == end || *cursor < '0' || *cursor > '9') {
            return false;
        }
        const auto parsed = std::from_chars(cursor, end, components[i]);
        if (parsed.ec != std::errc{}) {
            return false;
        }
        cursor = parsed.ptr;
        if (i + 1 != components.size()) {
            if (cursor == end || *cursor++ != '.') {
                return false;
            }
        }
    }
    return cursor == end && components[0] >= 23;
}

void RegisterStreamPlayServices(ServerManager& manager, Core::System& system);

} // namespace Service::NGC
