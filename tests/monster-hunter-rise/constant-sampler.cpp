#include <cstdio>

#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_video_core/textures/texture.h"
VideoSettings videoSettings{};
struct IModuleSettings;
IModuleSettings* g_settings{};
namespace {
class Environment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 7; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

int checks{}, failures{};

void Check(const char* name, bool result) {
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", name);
}
} // namespace

int main() {
    for (int kind = 0; kind < 8; ++kind) {
        Shader::ObjectPool<Shader::IR::Inst> pool{512};
        Shader::IR::Block block{pool};
        Shader::IR::IREmitter ir{block};
        Shader::IR::Program program{};
        program.stage = Shader::Stage::Fragment;
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        const auto base = ir.GetCbuf(ir.Imm32(2u), ir.Imm32(0x180u));
        const auto sample = [&](Shader::IR::U32 handle) {
            Shader::IR::TextureInstInfo flags{};
            flags.type.Assign(kind == 7 ? Shader::TextureType::Buffer
                                       : Shader::TextureType::ColorCube);
            auto coords = ir.CompositeConstruct(ir.Imm32(1.f), ir.Imm32(0.f), ir.Imm32(0.f));
            ir.Reference(ir.ImageSampleExplicitLod(handle, coords, ir.Imm32(4.f), {}, flags));
        };
        const u32 sampler = kind == 0 ? 0u : 267u << 20;
        if (kind == 2) {
            sample(ir.BitwiseOr(ir.Imm32(sampler), base));
        } else if (kind == 3) {
            sample(ir.BitwiseOr(ir.BitwiseOr(base, ir.Imm32(256u << 20)), ir.Imm32(11u << 20)));
        } else if (kind == 6) {
            auto second = ir.ShiftLeftLogical(ir.GetCbuf(ir.Imm32(2u), ir.Imm32(0x568u)),
                                              ir.Imm32(20u));
            sample(ir.BitwiseOr(ir.BitwiseOr(base, second), ir.Imm32(267u << 20)));
        } else {
            sample(kind == 0 ? base : ir.BitwiseOr(base, ir.Imm32(sampler)));
        }
        if (kind == 4) {
            sample(ir.BitwiseOr(base, ir.Imm32(sampler)));
        }
        if (kind == 5) {
            sample(ir.BitwiseOr(base, ir.Imm32(263u << 20)));
        }

        Environment env;
        Shader::HostTranslateInfo host{};
        host.max_bindless_descriptors_per_stage = 4096;
        Shader::Optimization::TexturePass(env, program, host);
        const auto& textures = program.info.texture_descriptors;
        const auto& buffers = program.info.texture_buffer_descriptors;
        if (kind == 7) {
            Check("buffer_handle_preserves_constant_bits",
                  buffers.size() == 1 && buffers[0].handle_or_bits == sampler);
            continue;
        }
        const auto& desc = textures.at(0);
        static const char* names[] = {
            "plain_cbuf_handle_unchanged", "immediate_sampler_267", "commuted_immediate_sampler",
            "nested_immediate_sampler", "identical_handles_share_descriptor",
            "distinct_samplers_keep_distinct_descriptors", "secondary_cbuf_plus_immediate_sampler",
        };
        bool valid = desc.cbuf_index == 2 && desc.cbuf_offset == 0x180 &&
                     desc.handle_or_bits == sampler;
        valid &= textures.size() == (kind == 5 ? 2u : 1u);
        if (kind == 5) {
            valid &= textures[1].handle_or_bits == (263u << 20);
        }
        if (kind == 6) {
            valid &= desc.has_secondary && desc.secondary_cbuf_offset == 0x568 &&
                     desc.secondary_shift_left == 20;
        }
        Check(names[kind], valid);
        const auto pair = Tegra::Texture::TexturePair(
            Tegra::Texture::ComposeTextureHandle(desc, 0x12345, kind == 6 ? 4 : 0), false);
        Check("runtime_texture_index_preserved", pair.first == 0x12345);
        Check("runtime_sampler_bits_preserved", pair.second == ((sampler >> 20) | (kind == 6 ? 4 : 0)));
    }
    Shader::TextureDescriptor linked{};
    linked.handle_or_bits = 267u << 20;
    const auto pair = Tegra::Texture::TexturePair(
        Tegra::Texture::ComposeTextureHandle(linked, 0x1234), true);
    Check("linked_sampler_uses_composed_handle",
          pair.first == (0x1234 | (267u << 20)) && pair.second == pair.first);
    std::printf("checks=%d failed=%d\n", checks, failures);
    return failures ? 1 : 0;
}
