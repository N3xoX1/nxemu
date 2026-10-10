// Emit real production SPIR-V for two invalid patterns found in the game captures.
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <exception>
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
VideoSettings videoSettings{};
struct IModuleSettings;
IModuleSettings* g_settings{};
class Environment final : public Shader::Environment {
public:
 u64 ReadInstruction(u32) override {return 0;}
 u32 ReadCbufValue(u32,u32) override {return 0;}
 u32 ReadCbufSize(u32) override {return 65536;}
#ifndef ORIGINAL_SHADER_BASELINE
 u32 ReadTextureTableSize() override {return 4;}
#endif
 Shader::TextureType ReadTextureType(u32) override {return Shader::TextureType::Color3D;}
 Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {return {};}
 bool IsTexturePixelFormatInteger(u32) override {return false;}
 u32 ReadViewportTransformState() override {return 1;}
 u32 TextureBoundBuffer() const override {return 0;}
 u32 LocalMemorySize() const override {return 1536;}
 u32 SharedMemorySize() const override {return 1536;}
 std::array<u32,3> WorkgroupSize() const override {return {1,1,1};}
 bool HasHLEMacroState() const override {return false;}
 std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32,u32) override {return {};}
 void Dump(u64,u64) override {}
};
int main(int argc,char** argv) try {
 if(argc!=2)return 2;
 std::filesystem::create_directories(argv[1]);
#ifdef ORIGINAL_SHADER_BASELINE
 constexpr int case_count=4;
#else
 constexpr int case_count=5;
#endif
 for(int kind=0;kind<case_count;++kind){
  std::fprintf(stderr,"Case %d constructing IR\n",kind);
  Shader::ObjectPool<Shader::IR::Inst> pool{64};
  Shader::IR::Block block{pool};
  Shader::IR::IREmitter ir{block};
  Shader::IR::Program program{};
  program.stage=Shader::Stage::Compute;program.workgroup_size={1,1,1};
  program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
  program.syntax_list.push_back({.data={.block=&block},.type=Shader::IR::AbstractSyntaxNode::Type::Block});
  program.syntax_list.push_back({.type=Shader::IR::AbstractSyntaxNode::Type::Return});
  if(kind<2){
   Shader::IR::TextureInstInfo flags{};
   flags.type.Assign(Shader::TextureType::Buffer);
   flags.image_format.Assign(Shader::ImageFormat::R32_UINT);
   const auto value=ir.ImageRead({},ir.Imm32(0u),flags);
   ir.Reference(value);
   program.info.image_buffer_descriptors.push_back({.format=Shader::ImageFormat::R32_UINT,
     .is_written=kind==1,.is_read=true,.is_integer=true,.count=1});
   if(kind==1)ir.ImageWrite({},ir.Imm32(0u),value,flags);
  }else if(kind<4){
   program.local_memory_size=1536;program.shared_memory_size=kind==2?1536:2048;
   ir.WriteLocal(ir.Imm32(0u),ir.Imm32(123u));
   ir.WriteShared(32,ir.Imm32(0u),ir.Imm32(456u));
  }else{
   program.workgroup_size={4,1,1};
   program.shared_memory_size=16;
   Shader::IR::TextureInstInfo flags{};
   flags.type.Assign(Shader::TextureType::Color3D);
   flags.image_format.Assign(Shader::ImageFormat::Typeless);
   const Shader::IR::U32 id{ir.CompositeExtract(ir.LocalInvocationId(),0)};
   const auto offset=Shader::IR::U32{ir.IMul(id,ir.Imm32(4u))};
   const auto raw_handle=ir.Select(ir.IEqual(id,ir.Imm32(3u)),ir.Imm32(0x00100005u),id);
   ir.WriteShared(32,offset,raw_handle);
   const auto handle=Shader::IR::U32{ir.LoadShared(32,false,offset)};
   const auto color=ir.Select(ir.IEqual(id,ir.Imm32(0u)),ir.Imm32(0x3f800000u),
       ir.Select(ir.IEqual(id,ir.Imm32(1u)),ir.Imm32(42u),ir.Imm32(0xfffffff9u)));
   ir.ImageWrite(handle,ir.CompositeConstruct(ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u)),
       ir.CompositeConstruct(color,ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u)),flags);
  }
  Environment env;
#ifndef ORIGINAL_SHADER_BASELINE
  if(kind==4){
   Shader::HostTranslateInfo host{};host.max_storage_images_per_stage=15;
   host.support_null_descriptor=true;
   Shader::Optimization::TexturePass(env,program,host);
   if(program.info.image_descriptors.size()!=3 || !program.info.image_descriptors[0].is_direct ||
      program.info.image_descriptors[0].count!=5 || program.info.runtime_image_write_mask_words!=1)
      throw std::runtime_error("Runtime TIC lowering failed");
  }
#endif
  // Texture lowering normally replaces these frontend operations before collection.
  for(auto& inst:block.Instructions()) {
   if(inst.GetOpcode()==Shader::IR::Opcode::BindlessImageRead ||
      inst.GetOpcode()==Shader::IR::Opcode::BoundImageRead) {
    inst.ReplaceOpcode(Shader::IR::Opcode::ImageRead);
    inst.SetArg(0,{});
   } else if(inst.GetOpcode()==Shader::IR::Opcode::BindlessImageWrite ||
             inst.GetOpcode()==Shader::IR::Opcode::BoundImageWrite) {
    inst.ReplaceOpcode(Shader::IR::Opcode::ImageWrite);
    inst.SetArg(0,{});
   }
  }
  std::fprintf(stderr,"Case %d collecting info\n",kind);
  Shader::Optimization::CollectShaderInfoPass(env,program);
  Shader::Profile profile{};
  profile.unified_descriptor_binding=true;
  profile.supported_spirv=0x00010600;
  profile.support_explicit_workgroup_layout=kind<4;
#ifndef ORIGINAL_SHADER_BASELINE
  profile.support_storage_image_array_nonuniform_indexing=true;
#endif
  std::fprintf(stderr,"Case %d emitting SPIR-V\n",kind);
  const auto words=Shader::Backend::SPIRV::EmitSPIRV(profile,program);
  const auto path=std::filesystem::path(argv[1])/((std::to_string(kind))+".spv");
  std::ofstream file(path,std::ios::binary);
  file.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(u32));
  std::printf("EMITTED %d words=%zu image_buffer=%d\n",kind,words.size(),program.info.uses_image_buffers);
 }
}catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;
}
