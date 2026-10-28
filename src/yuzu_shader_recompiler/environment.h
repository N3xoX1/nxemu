// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/program_header.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_shader_recompiler/stage.h"

namespace Shader {

enum class TextureQueryKind { Type, Format, Integer };

/// Sizes of the bound texture (TIC) and sampler (TSC) tables.
struct TexturePoolInfo {
    u32 num_textures{};
    u32 num_samplers{};
    /// The sampler is selected by the texture index instead of the handle's sampler field.
    bool linked_samplers{};
};

class Environment {
public:
    virtual ~Environment() = default;

    [[nodiscard]] virtual u64 ReadInstruction(u32 address) = 0;

    [[nodiscard]] virtual u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) = 0;

    // Preserve the source of a handle used only to classify a texture. Literal
    // constant folding and branch-table reads still use ReadCbufValue directly.
    [[nodiscard]] u32 ResolveTextureHandle(const TextureHandleSource& source) {
        const auto bank = source.has_secondary ? source.secondary_cbuf_index : source.cbuf_index;
        const auto offset = source.has_secondary ? source.secondary_cbuf_offset : source.cbuf_offset;
        return (ReadCbufValue(source.cbuf_index, source.cbuf_offset) << source.shift_left) |
               (ReadCbufValue(bank, offset) << source.secondary_shift_left) | source.handle_or_bits;
    }
    [[nodiscard]] virtual u32 ReadTextureQuery(const TextureHandleSource& source, TextureQueryKind kind) {
        return QueryTextureHandle(ResolveTextureHandle(source), kind);
    }
    [[nodiscard]] u32 QueryTextureHandle(u32 handle, TextureQueryKind kind) {
        switch (kind) {
        case TextureQueryKind::Type: return static_cast<u32>(ReadTextureType(handle));
        case TextureQueryKind::Format: return static_cast<u32>(ReadTexturePixelFormat(handle));
        case TextureQueryKind::Integer: return IsTexturePixelFormatInteger(handle);
        }
        return 0;
    }

    // Zero means this environment cannot provide the bound size (e.g. an old cache).
    [[nodiscard]] virtual u32 ReadCbufSize([[maybe_unused]] u32 cbuf_index) { return 0; }

    // Number of entries in the compute TIC table. Zero means unavailable.
    [[nodiscard]] virtual u32 ReadTextureTableSize() { return 0; }

    // Tables indexed by texture handles that are only known when the shader runs.
    // Empty means this environment cannot provide them.
    [[nodiscard]] virtual std::optional<TexturePoolInfo> ReadTexturePoolInfo() {
        return std::nullopt;
    }

    [[nodiscard]] virtual TextureType ReadTextureType(u32 raw_handle) = 0;

    [[nodiscard]] virtual TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) = 0;

    [[nodiscard]] virtual bool IsTexturePixelFormatInteger(u32 raw_handle) = 0;

    [[nodiscard]] virtual u32 ReadViewportTransformState() = 0;

    [[nodiscard]] virtual u32 TextureBoundBuffer() const = 0;

    [[nodiscard]] virtual u32 LocalMemorySize() const = 0;

    [[nodiscard]] virtual u32 SharedMemorySize() const = 0;

    [[nodiscard]] virtual std::array<u32, 3> WorkgroupSize() const = 0;

    [[nodiscard]] virtual bool HasHLEMacroState() const = 0;

    [[nodiscard]] virtual std::optional<ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) = 0;

    virtual void Dump(u64 pipeline_hash, u64 shader_hash) = 0;

    [[nodiscard]] const ProgramHeader& SPH() const noexcept {
        return sph;
    }

    [[nodiscard]] const std::array<u32, 8>& GpPassthroughMask() const noexcept {
        return gp_passthrough_mask;
    }

    [[nodiscard]] Stage ShaderStage() const noexcept {
        return stage;
    }

    [[nodiscard]] u32 StartAddress() const noexcept {
        return start_address;
    }

    [[nodiscard]] bool IsProprietaryDriver() const noexcept {
        return is_proprietary_driver;
    }

protected:
    ProgramHeader sph{};
    std::array<u32, 8> gp_passthrough_mask{};
    Stage stage{};
    u32 start_address{};
    bool is_proprietary_driver{};
};

} // namespace Shader
