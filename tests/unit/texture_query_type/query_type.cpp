// SPDX-License-Identifier: GPL-2.0-or-later
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include <cstdio>
#include <vector>

using namespace Shader;
VideoSettings videoSettings{};

class Environment final : public Shader::Environment {
public:
  explicit Environment(TextureType bound_type_) : bound_type{bound_type_} {
    stage = Stage::Fragment;
  }
  TextureType bound_type;
  u64 ReadInstruction(u32) override { return 0; }
  u32 ReadCbufValue(u32, u32) override { return 0; }
  u32 ReadCbufSize(u32) override { return 0x1000; }
  TextureType ReadTextureType(u32) override { return bound_type; }
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

constexpr u32 QUERIED_HANDLE = 0x40;
constexpr u32 OTHER_HANDLE = 0x48;

enum class Op { Query, Sample, Fetch, QueryLod };
struct Use {
  Op op;
  TextureType type{};
  u32 handle{QUERIED_HANDLE};
};

TextureType FirstQueryType(const std::vector<Use> &uses, TextureType bound_type) {
  ObjectPool<IR::Inst> pool;
  IR::Block block{pool};
  IR::IREmitter ir{block};
  ir.Prologue();
  IR::Inst *query{};
  for (const Use &use : uses) {
    IR::TextureInstInfo flags{};
    flags.type.Assign(use.type);
    const IR::Value handle{ir.Imm32(use.handle)};
    const IR::Value coords{
        ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0), ir.Imm32(0))};
    switch (use.op) {
    case Op::Query: {
      const IR::Value value{
          ir.ImageQueryDimension(handle, ir.Imm32(0), ir.Imm1(false))};
      if (!query)
        query = value.Inst();
      break;
    }
    case Op::Sample:
      (void)ir.ImageSampleExplicitLod(handle, coords, ir.Imm32(0.0f), {},
                                      flags);
      break;
    case Op::Fetch:
      (void)ir.ImageFetch(handle, coords, {}, ir.Imm32(0), {}, flags);
      break;
    case Op::QueryLod:
      (void)ir.ImageQueryLod(handle, coords, flags);
      break;
    }
  }
  ir.Epilogue();
  IR::Program program{};
  program.stage = Stage::Fragment;
  program.blocks = {&block};
  program.post_order_blocks = {&block};
  program.syntax_list = {
      {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block},
      {.type = IR::AbstractSyntaxNode::Type::Return},
  };
  ::Environment env{bound_type};
  HostTranslateInfo host{};
  host.support_separate_sampler = true;
  Optimization::TexturePass(env, program, host);
  return query->Flags<IR::TextureInstInfo>().type;
}

void Check(const char *name, const std::vector<Use> &uses,
           TextureType bound_type, TextureType expected) {
  const TextureType type{FirstQueryType(uses, bound_type)};
  if (type != expected) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s: type %d, expected %d\n", name,
                 static_cast<int>(type), static_cast<int>(expected));
  }
}

int main() {
  using enum TextureType;
  Check("matching siblings override a stale descriptor (Pokemon Z-A)",
        {{Op::Sample, Color2D}, {Op::Query}, {Op::Sample, Color2D}}, Color1D,
        Color2D);
  Check("conflicting siblings",
        {{Op::Sample, Color2D}, {Op::Query}, {Op::Sample, ColorArray2D}},
        ColorCube, ColorCube);
  Check("conflicting siblings, reversed",
        {{Op::Sample, ColorArray2D}, {Op::Query}, {Op::Sample, Color2D}},
        ColorCube, ColorCube);
  Check("no sibling", {{Op::Query}}, Color3D, Color3D);
  Check("no eligible sibling",
        {{Op::Query},
         {Op::Query},
         {Op::Fetch, Color1D},
         {Op::Fetch, Buffer},
         {Op::QueryLod, Color2D},
         {Op::Sample, Color2D, OTHER_HANDLE}},
        Color3D, Color3D);
  Check("ineligible siblings do not conflict",
        {{Op::Fetch, Color1D},
         {Op::QueryLod, ColorArray2D},
         {Op::Fetch, Buffer},
         {Op::Sample, ColorArray2D, OTHER_HANDLE},
         {Op::Query},
         {Op::Sample, Color2D}},
        Color1D, Color2D);
  if (failures != 0)
    return 1;
  std::puts("texture query type tests passed");
  return 0;
}
