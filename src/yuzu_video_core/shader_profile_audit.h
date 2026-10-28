// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_video_core/shader_environment.h"
#include <utility>
#include "yuzu_video_core/shader_profile_hash.h"
#include <span>
#include <vector>

namespace VideoCommon::ShaderProfile::Audit
{
// Offline validation against captured game inputs. UINT32_MAX means the bank
// was demonstrably read but its full size was not captured: it satisfies only
// the enabled-bank test and cannot match an actual guest size dependency.
class CapturedEnvironment final : public Shader::Environment {
    FileEnvironment& source;
    bool hle;
public:
    CapturedEnvironment(FileEnvironment& env,bool hle_) : source(env),hle(hle_) {
        sph=env.SPH();gp_passthrough_mask=env.GpPassthroughMask();
        stage=env.ShaderStage();start_address=env.StartAddress();
    }
    u64 ReadInstruction(u32 address) override {return source.ReadInstruction(address);}
    u32 ReadCbufValue(u32 bank,u32 offset) override {return source.ReadCbufValue(bank,offset);}
    u32 ReadCbufSize(u32 bank) override {
        const auto size=source.ReadCbufSize(bank);
        return size?size:source.HasCapturedCbufBank(bank)?UINT32_MAX:0;
    }
    u32 ReadTextureTableSize() override {return source.ReadTextureTableSize();}
    std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override {return source.ReadTexturePoolInfo();}
    Shader::TextureType ReadTextureType(u32 handle) override {return source.ReadTextureType(handle);}
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {return source.ReadTexturePixelFormat(handle);}
    bool IsTexturePixelFormatInteger(u32 handle) override {return source.IsTexturePixelFormatInteger(handle);}
    u32 ReadViewportTransformState() override {return source.ReadViewportTransformState();}
    u32 TextureBoundBuffer() const override {return source.TextureBoundBuffer();}
    u32 LocalMemorySize() const override {return source.LocalMemorySize();}
    u32 SharedMemorySize() const override {return source.SharedMemorySize();}
    std::array<u32,3> WorkgroupSize() const override {return source.WorkgroupSize();}
    bool HasHLEMacroState() const override {return hle;}
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,u32 offset) override {return source.GetReplaceConstBuffer(bank,offset);}
    void Dump(u64,u64) override {}
};
// Enabled only by an explicit offline startup audit. Reports contain hashes,
// never SPIR-V or guest instructions. Normal draw execution does not use this.
struct Capture {
    std::vector<std::string> spirv;
    std::vector<std::pair<size_t, Shader::Info>> interfaces;
    bool interfaces_captured{};
    bool has_vtg{};
    bool rasterizes_lines{};
    bool operator==(const Capture&) const = default;
};
inline thread_local Capture* current{};
inline void Record(std::span<const u32> code)
{
    if (current) current->spirv.push_back(ResourceSha256(std::string_view(
        reinterpret_cast<const char *>(code.data()), code.size_bytes())));
}
class Context
{
public:
    explicit Context(Capture & hashes) :
        previous(current)
    {
        current = &hashes;
    }
    ~Context()
    {
        current = previous;
    }
    Context(const Context &) = delete;

private:
    Capture* previous;
};
} // namespace VideoCommon::ShaderProfile::Audit
