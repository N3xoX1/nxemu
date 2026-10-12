// Emit production SPIR-V for a fragment shader sampling a texture handle read from a varying.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/runtime_info.h"
VideoSettings videoSettings{};
struct IModuleSettings;
IModuleSettings* g_settings{};

class Environment final : public Shader::Environment {
public:
    explicit Environment(bool linked_, bool has_pool_) : linked{linked_}, has_pool{has_pool_} {
        stage = Shader::Stage::Fragment;
    }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { throw std::runtime_error("Unexpected cbuf read"); }
    std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override {
        if (!has_pool) {
            return std::nullopt;
        }
        return Shader::TexturePoolInfo{
            .num_textures = 300, .num_samplers = linked ? 300U : 64U, .linked_samplers = linked};
    }
    Shader::TextureType ReadTextureType(u32) override {
        throw std::runtime_error("Unexpected texture type read");
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}

private:
    bool linked;
    bool has_pool;
};

// Cases: 0 separate samplers, 1 linked samplers, 2 host without separate samplers,
// 3 environment without pool sizes. The last two must keep failing translation.
int main(int argc, char** argv) try {
    if (argc != 2) {
        return 2;
    }
    std::filesystem::create_directories(argv[1]);
    for (int kind = 0; kind < 4; ++kind) {
        Shader::ObjectPool<Shader::IR::Inst> pool{64};
        Shader::IR::Block block{pool};
        Shader::IR::IREmitter ir{block};
        Shader::IR::Program program{};
        program.stage = Shader::Stage::Fragment;
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back(
            {.data = {.block = &block}, .type = Shader::IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = Shader::IR::AbstractSyntaxNode::Type::Return});

        Shader::IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        const Shader::IR::U32 handle{
            ir.BitCast<Shader::IR::U32>(ir.GetAttribute(Shader::IR::Attribute::Generic2X))};
        const auto coords = ir.CompositeConstruct(ir.GetAttribute(Shader::IR::Attribute::Generic0X),
                                                  ir.GetAttribute(Shader::IR::Attribute::Generic0Y));
        // Two uses of the same handle must share one table.
        for (u32 component = 0; component < 2; ++component) {
            const auto color = ir.ImageSampleExplicitLod(handle, coords, ir.Imm32(0.0f), {}, flags);
            ir.SetFragColor(0, component,
                            Shader::IR::F32{ir.CompositeExtract(color, component)});
        }

        Environment env{kind == 1, kind != 3};
        Shader::HostTranslateInfo host{};
        host.max_bindless_descriptors_per_stage = 1U << 20;
        host.support_separate_sampler = kind != 2;
        bool failed{};
        try {
            Shader::Optimization::TexturePass(env, program, host);
        } catch (const Shader::Exception&) {
            failed = true;
        }
        if (kind >= 2) {
            if (!failed) {
                throw std::runtime_error("Unsupported case was translated");
            }
            std::printf("REJECTED %d\n", kind);
            continue;
        }
        const auto& info = program.info;
        if (failed || info.texture_descriptors.size() != 1 ||
            !info.texture_descriptors[0].is_direct || info.texture_descriptors[0].count != 300 ||
            info.runtime_sampler_count != (kind == 1 ? 300U : 64U) ||
            info.runtime_samplers_linked != (kind == 1)) {
            throw std::runtime_error("Runtime handle lowering failed");
        }
        Shader::Optimization::CollectShaderInfoPass(env, program);
        Shader::Profile profile{};
        profile.unified_descriptor_binding = true;
        profile.supported_spirv = 0x00010600;
        profile.support_sampled_image_array_nonuniform_indexing = true;
        Shader::RuntimeInfo runtime_info{};
        for (const auto attribute :
             {Shader::IR::Attribute::Generic0X, Shader::IR::Attribute::Generic0Y,
              Shader::IR::Attribute::Generic2X}) {
            runtime_info.previous_stage_stores.Set(attribute);
        }
        Shader::Backend::Bindings bindings{};
        const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
        if (bindings.texture_table != 2 || bindings.unified != 0) {
            throw std::runtime_error("Tables were not bound in their own set");
        }
        const auto path = std::filesystem::path(argv[1]) / (std::to_string(kind) + ".spv");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
        std::printf("EMITTED %d words=%zu\n", kind, words.size());
    }
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
