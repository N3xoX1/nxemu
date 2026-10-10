// Replay captured Maxwell compute code through the real shader translator.
// Resource metadata is synthetic; this isolates handle tracking, not game rendering.
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"

VideoSettings videoSettings{};
struct IModuleSettings;
IModuleSettings* g_settings{};

class ReplayEnvironment final : public Shader::Environment {
public:
    explicit ReplayEnvironment(const char* path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (!file || size <= 0 || size % sizeof(u64)) throw std::runtime_error("Invalid code file");
        code.resize(static_cast<size_t>(size) / sizeof(u64));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), size);
        if (!file) throw std::runtime_error("Truncated code file");
        stage = Shader::Stage::Compute;
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        if (address % sizeof(u64) || address / sizeof(u64) >= code.size())
            throw std::runtime_error("Instruction outside captured code");
        return code[address / sizeof(u64)];
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    u32 ReadCbufSize(u32) override { return 65536; }
    u32 ReadTextureTableSize() override { return 64; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return true; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 4096; }
    u32 SharedMemorySize() const override { return 49152; }
    std::array<u32, 3> WorkgroupSize() const override { return {8, 8, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
private:
    std::vector<u64> code;
};

int main(int argc, char** argv) try {
    if (argc < 2) throw std::runtime_error("Usage: shader-replay.exe captured.ash ...");
    for (int i = 1; i < argc; ++i) {
        ReplayEnvironment env(argv[i]);
        Shader::ObjectPool<Shader::IR::Inst> insts{8192};
        Shader::ObjectPool<Shader::IR::Block> blocks{32};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks{32};
        Shader::Maxwell::Flow::CFG cfg(env, flow_blocks, 0);
        Shader::HostTranslateInfo host{};
        host.support_int64 = true;
        host.support_float64 = true;
        host.max_bindless_descriptors_per_stage = 1048576;
        host.max_storage_images_per_stage = 1048576;
        host.support_null_descriptor = true;
        auto program = Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host);
        Shader::Profile profile{};
        profile.unified_descriptor_binding = true;
        profile.supported_spirv = 0x00010600;
        profile.support_int64 = true;
        profile.support_vote = true;
        profile.support_typeless_image_loads = true;
        profile.support_sampled_image_array_nonuniform_indexing = true;
        profile.support_storage_image_array_nonuniform_indexing = true;
        profile.support_explicit_workgroup_layout = true;
        const auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        std::ofstream output(std::string(argv[i])+".spv", std::ios::binary);
        output.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(u32));
        std::printf("PASS %s: blocks=%zu textures=%zu images=%zu\n", argv[i],
                    program.blocks.size(), program.info.texture_descriptors.size(),
                    program.info.image_descriptors.size());
    }
    return 0;
} catch (const std::exception& exception) {
    std::printf("FAIL %s\n", exception.what());
    return 1;
}
