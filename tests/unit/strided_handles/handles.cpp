// SPDX-License-Identifier: GPL-2.0-or-later
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/backend/glsl/emit_glsl.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/bindless_descriptor.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace Shader;
VideoSettings videoSettings{};
std::filesystem::path emit_directory;
class Environment final : public Shader::Environment {
public:
  Environment() { stage = Stage::Fragment; }
  u32 size = 16384;
  u64 ReadInstruction(u32) override { return 0; }
  u32 ReadCbufValue(u32, u32 offset) override {
    if (offset > size || size - offset < 4)
      std::abort();
    return 0;
  }
  u32 ReadCbufSize(u32) override { return size; }
  TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
  TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
  bool IsTexturePixelFormatInteger(u32) override { return false; }
  u32 ReadViewportTransformState() override { return 0; }
  u32 TextureBoundBuffer() const override { return 2; }
  u32 LocalMemorySize() const override { return 0; }
  u32 SharedMemorySize() const override { return 0; }
  std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
  bool HasHLEMacroState() const override { return false; }
  std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
    return {};
  }
  void Dump(u64, u64) override {}
};

unsigned failures{};
double pass_ns{};
void Check(bool valid, const char *message) {
  if (!valid) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

// Evaluate the production-generated descriptor index with an independent guest
// input.
u32 Evaluate(IR::Value value, u32 input) {
  value = value.Resolve();
  if (value.IsImmediate())
    return value.U32();
  const auto *inst = value.Inst();
  const auto arg = [&](size_t i) { return Evaluate(inst->Arg(i), input); };
  switch (inst->GetOpcode()) {
  case IR::Opcode::GetCbufU32:
    return input;
  case IR::Opcode::IAdd32:
    return arg(0) + arg(1);
  case IR::Opcode::IMul32:
    return arg(0) * arg(1);
  case IR::Opcode::BitwiseAnd32:
    return arg(0) & arg(1);
  case IR::Opcode::UMin32:
    return std::min(arg(0), arg(1));
  case IR::Opcode::ShiftLeftLogical32:
    return arg(0) << arg(1);
  case IR::Opcode::ShiftRightLogical32:
    return arg(0) >> arg(1);
  case IR::Opcode::FindUMsb32:
    return arg(0) ? 31U - std::countl_zero(arg(0)) : ~0U;
  default:
    std::abort();
  }
}

void Case(u32 stride, u32 base, unsigned mode, bool shift = false) {
  ObjectPool<IR::Inst> pool;
  IR::Block block{pool};
  IR::IREmitter ir{block};
  ir.Prologue();
  auto index = ir.GetCbuf(ir.Imm32(3), ir.Imm32(0));
  if (mode == 1)
    index = ir.BitwiseAnd(index, ir.Imm32(31));
  if (mode == 2)
    index = ir.FindUMsb(index);
  IR::U32 offset = shift ? IR::U32{ir.ShiftLeftLogical(
                               index, ir.Imm32(std::countr_zero(stride)))}
                         : ir.IMul(index, ir.Imm32(stride));
  if (base)
    offset = ir.IAdd(ir.Imm32(base), offset);
  const auto handle = ir.GetCbuf(ir.Imm32(2), offset);
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  const auto fetch =
      ir.ImageFetch(handle, ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)), {},
                    ir.Imm32(0), {}, flags);
  for (u32 component = 0; component < 4; ++component)
    ir.SetFragColor(0, component,
                    IR::F32{ir.CompositeExtract(fetch, component)});
  ir.Epilogue();
  IR::Program program{};
  program.stage = Stage::Fragment;
  program.blocks = {&block};
  program.post_order_blocks = {&block};
  program.syntax_list = {
      {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block},
      {.type = IR::AbstractSyntaxNode::Type::Return},
  };
  ::Environment env;
  HostTranslateInfo host{};
  host.support_separate_sampler = true;
  host.max_bindless_descriptors_per_stage = 32768;
  const auto start = std::chrono::steady_clock::now();
  Optimization::TexturePass(env, program, host);
  pass_ns += std::chrono::duration<double, std::nano>(
                 std::chrono::steady_clock::now() - start)
                 .count();
  Check(program.info.texture_descriptors.size() == 1, "one descriptor");
  const auto &desc = program.info.texture_descriptors.front();
  Check(u64(desc.cbuf_offset) + u64(desc.count - 1) * desc.stride + 4 <=
            env.size,
        "all uploaded handles fit the cbuf");
  if (mode == 1 || (mode == 2 && base == 0)) {
    Check(desc.stride == stride && desc.count == 32, "compact bounded array");
  }
  const auto test = [&](u32 input) {
    const u32 guest_offset = Evaluate(offset, input);
    if (guest_offset > env.size - 4)
      return;
    // A draw-uniform index is read from its cbuf when the pipeline binds.
    const u32 uniform = desc.has_index_cbuf ? input << desc.index_shift : 0;
    const u32 selected =
        desc.count == 1 ? 0 : Evaluate(fetch.Inst()->Arg(0), input);
    const u32 host_offset = desc.cbuf_offset + uniform + selected * desc.stride;
    if (host_offset != guest_offset) {
      ++failures;
      std::fprintf(
          stderr,
          "FAIL: stride=%u base=%u mode=%u input=%08x guest=%u host=%u\n",
          stride, base, mode, input, guest_offset, host_offset);
    }
  };
  for (u32 i = 0; i < 64; ++i)
    test(i);
  for (u32 i : {0xffffffffU, 0x80000000U, 0x40000001U, 0xfffffffeU})
    test(i);
  if (!emit_directory.empty()) {
    try {
      Optimization::DeadCodeEliminationPass(program);
      Optimization::CollectShaderInfoPass(env, program);
      Profile profile{};
      profile.supported_spirv = 0x00010300;
      profile.support_sampled_image_array_nonuniform_indexing = true;
      RuntimeInfo runtime{};
      Backend::Bindings bindings{};
      const auto words =
          Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
      const auto name = std::to_string(stride) + "-" + std::to_string(base) +
                        "-" + std::to_string(mode) + (shift ? "-shift" : "");
      std::ofstream binary(emit_directory / (name + ".spv"), std::ios::binary);
      binary.write(reinterpret_cast<const char *>(words.data()),
                   words.size() * sizeof(u32));
      bindings = {};
      const auto glsl =
          Backend::GLSL::EmitGLSL(profile, runtime, program, bindings);
      std::ofstream(emit_directory / (name + ".frag")) << glsl;
    } catch (const std::exception &e) {
      std::fprintf(stderr, "Shader emission: %s\n", e.what());
      std::exit(1);
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 3) {
    emit_directory = argv[2];
    std::filesystem::create_directories(emit_directory);
  }
  for (u32 stride : {4U, 8U, 16U, 192U, 304U}) {
    for (u32 base : {0U, 512U})
      for (unsigned mode = 0; mode < 3; ++mode)
        Case(stride, base, mode);
  }
  Case(16, 0, 0, true);
  Case(16, 512, 0, true);
  using Optimization::BindlessDescriptorCountStrided;
  Check(BindlessDescriptorCountStrided(3, 0, 4) == 0, "truncated word");
  Check(BindlessDescriptorCountStrided(16, 17, 4) == 0, "base beyond buffer");
  Check(BindlessDescriptorCountStrided(16, 0, 0) == 0, "zero stride");
  Check(BindlessDescriptorCountStrided(612, 0, 304) == 3,
        "last complete handle");
  if (argc == 2) {
    for (unsigned mode : {1U, 0U}) {
      for (unsigned i = 0; i < 100; ++i)
        Case(304, 0, mode);
      pass_ns = 0;
      for (unsigned i = 0; i < 10000; ++i)
        Case(304, 0, mode);
      std::printf("304-byte records, %s index: TexturePass mean %.1f ns\n",
                  mode ? "bounded" : "unbounded", pass_ns / 10000);
    }
  }
  std::printf("strided handles: %u failures\n", failures);
  return failures ? 1 : 0;
}
