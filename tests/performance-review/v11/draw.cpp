
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using f32=float;
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET=0,RESCALING_LAYOUT_DOWN_FACTOR_OFFSET=24,RENDERAREA_LAYOUT_OFFSET=0;
struct {struct {float down_factor=.5f;} resolution_info;} videoSettings;
struct Trace {std::array<u32,7> push_state{};std::vector<u64> events;u64 checksum{};bool verbose=true;void add(u64 x){checksum+=x;if(verbose)events.push_back(x);}};
namespace Common {template<class T>constexpr T AlignUp(T v,T a){return(v+a-1)&~(a-1);}}
namespace vk {
struct CommandBuffer {
 Trace* trace{};VkCommandBuffer handle{};mutable size_t push_calls{};void Snapshot()const{trace->add(99);for(auto word:trace->push_state)trace->add(word);}
 void BindPipeline(VkPipelineBindPoint point,VkPipeline p)const{trace->add(1);trace->add(point);trace->add(uintptr_t(p));if(handle)vkCmdBindPipeline(handle,point,p);}
 void PushConstants(VkPipelineLayout layout,VkShaderStageFlags stages,u32 offset,size_t size,const void* data)const{++push_calls;for(size_t i=0;i<size/4;++i){u32 word;std::memcpy(&word,static_cast<const char*>(data)+i*4,4);trace->push_state.at(offset/4+i)=word;}if(handle)vkCmdPushConstants(handle,layout,stages,offset,static_cast<u32>(size),data);}
 void BindDescriptorSets(VkPipelineBindPoint,VkPipelineLayout layout,u32 first,VkDescriptorSet set,const void*)const{trace->add(3);trace->add(uintptr_t(layout));trace->add(first);trace->add(uintptr_t(set));}
 void PushDescriptorSetWithTemplateKHR(VkDescriptorUpdateTemplate t,VkPipelineLayout layout,u32 first,const void* data)const{trace->add(4);trace->add(uintptr_t(t));trace->add(uintptr_t(layout));trace->add(first);trace->add(*static_cast<const u32*>(data));}
};
struct Device {
 Trace* trace{};
 void UpdateDescriptorSets(const boost::container::small_vector<VkWriteDescriptorSet,4>& writes,std::nullptr_t)const{trace->add(5);for(const auto& w:writes){trace->add(w.dstBinding);trace->add(w.descriptorCount);for(unsigned i=0;i<w.descriptorCount;++i){trace->add(uintptr_t(w.pImageInfo[i].imageView));trace->add(uintptr_t(w.pImageInfo[i].sampler));}}}
 void UpdateDescriptorSet(VkDescriptorSet set,VkDescriptorUpdateTemplate t,const void* data)const{trace->add(6);trace->add(uintptr_t(set));trace->add(uintptr_t(t));trace->add(*static_cast<const u32*>(data));}
};
}
struct Harness {
#if MATRIX < 8 && (MATRIX & 4)
#include "command-chunk-fused.inc"
#else
#include "command-chunk.inc"
#endif
 CommandChunk chunk;vk::CommandBuffer cmdbuf;size_t bytes{},records{},rollovers{};
 template<class T>void Record(T&& c){auto wrapper=[c=std::move(c)](vk::CommandBuffer main,vk::CommandBuffer){c(main);main.Snapshot();};if(!chunk.Record(wrapper)){Execute();++rollovers;if(!chunk.Record(wrapper))throw std::runtime_error("too large");}++records;}
 void Execute(){bytes+=chunk.command_offset;chunk.ExecuteAll(cmdbuf,{});}
};
#if MATRIX < 8 && (MATRIX & 4)
#include "command-execute-fused.inc"
#else
#include "command-execute.inc"
#endif
struct Rescaling{std::array<u32,6> words{};const auto& Data()const{return words;}};
struct RenderArea{bool uses_render_area{};std::array<f32,4> words{};};
struct Allocator{uintptr_t n=1000;VkDescriptorSet Commit(){return reinterpret_cast<VkDescriptorSet>(++n);}};
struct TemplateHandle{VkDescriptorUpdateTemplate handle=reinterpret_cast<VkDescriptorUpdateTemplate>(30);explicit operator bool()const{return handle!=nullptr;}VkDescriptorUpdateTemplate operator*()const{return handle;}TemplateHandle& operator=(std::nullptr_t){handle=nullptr;return *this;}};
struct Pipeline {
 Trace trace;Harness scheduler;bool uses_texture_table{},uses_push_descriptor{},built=true;
 VkPipeline object=reinterpret_cast<VkPipeline>(10);VkPipeline* pipeline=&object;
 VkPipelineLayout layout=reinterpret_cast<VkPipelineLayout>(20);VkPipelineLayout* pipeline_layout=&layout;
 TemplateHandle descriptor_update_template;
 std::shared_ptr<const std::vector<VkDescriptorImageInfo>> texture_table_data;u64 texture_table_version=1,texture_table_set_version=0;
 VkDescriptorSet texture_table_set{};Allocator texture_table_allocator,descriptor_allocator;
 std::vector<VkDescriptorSetLayoutBinding> texture_table_bindings;
 struct Dev{vk::Device logical;const auto& GetLogical()const{return logical;}} device;
 bool IsBuilt()const{return built;}
 Pipeline(){scheduler.cmdbuf.trace=&trace;device.logical.trace=&trace;texture_table_bindings={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};NewTable(1);}
 void NewTable(unsigned i){std::vector<VkDescriptorImageInfo> v(2);for(unsigned j=0;j<2;++j){v[j].sampler=reinterpret_cast<VkSampler>(uintptr_t(i*10+j));v[j].imageView=reinterpret_cast<VkImageView>(uintptr_t(i*100+j));}texture_table_data=std::make_shared<const std::vector<VkDescriptorImageInfo>>(std::move(v));++texture_table_version;}
 void Old(const Rescaling& rescaling,const RenderArea& render_area,const void* descriptor_data,bool bind_pipeline,bool is_rescaling,bool update_rescaling){
#include "reference-draw.inc"
 }
 void New(const Rescaling& rescaling,const RenderArea& render_area,const void* descriptor_data,bool bind_pipeline,bool is_rescaling,bool update_rescaling){
#define STRING2(x) #x
#define STRING(x) STRING2(x)
#include "candidate-draw.inc"
 }
};
void Require(bool x,const char* s){if(!x)throw std::runtime_error(s);}
volatile u64 sink;
int main()try{
 size_t cases=0;
 for(unsigned mode=0;mode<32;++mode){Pipeline a,b;a.uses_texture_table=b.uses_texture_table=bool(mode&1);a.uses_push_descriptor=b.uses_push_descriptor=bool(mode&2);a.built=b.built=!(mode&4);if(mode&8)a.descriptor_update_template=b.descriptor_update_template=nullptr;Rescaling rs;RenderArea area;u32 descriptor=12345;
  for(unsigned i=0;i<1500;++i){for(unsigned w=0;w<rs.words.size();++w)rs.words[w]=(i*0x9e3779b9u)^(w==0?0x7fc00000u:w==1?0x7f800000u:w==2?0x80000000u:w);area.uses_render_area=bool(mode&16);area.words={float(i),2,3,4};if(i%11==0){a.NewTable(i+1);b.NewTable(i+1);}a.Old(rs,area,&descriptor,i%5==0,i%2,i%7==0);b.New(rs,area,&descriptor,i%5==0,i%2,i%7==0);++cases;}
  a.scheduler.Execute();b.scheduler.Execute();Require(a.trace.events==b.trace.events,"driver trace differs");Require(a.texture_table_set_version==b.texture_table_set_version,"table version differs");Require(a.texture_table_data.use_count()==1&&b.texture_table_data.use_count()==1,"captured table leaked");Require(a.scheduler.records==b.scheduler.records,"record count differs");
  if(mode==0||mode==1)std::cout<<"BYTES table="<<bool(mode&1)<<' '<<a.scheduler.bytes<<' '<<b.scheduler.bytes<<" rollovers="<<a.scheduler.rollovers<<' '<<b.scheduler.rollovers<<'\n';
 }
 {
  Trace tr;Harness h;h.cmdbuf.trace=&tr;auto owner=std::make_shared<unsigned>(0);
  for(unsigned i=0;i<4000;++i)h.Record([owner,i](vk::CommandBuffer cb){++*owner;cb.trace->add(i);});
  h.Execute();Require(*owner==4000 && owner.use_count()==1,"shared ownership/rollover differs");
  Require(tr.events.size()==36000 && tr.events[35991]==3999,"command order differs");
 }
 std::cout<<"PASS "<<cases<<" deferred actual draw closures; final push-constant state and descriptor/pipeline trace identity; table ownership, updates, empty descriptors, failed builds, push/nonpush paths, chunk rollovers\n";
 for(unsigned scenario=0;scenario<8;++scenario){bool table=scenario&1;std::array<std::vector<double>,2> values;
  for(unsigned round=0;round<11;++round)for(unsigned order=0;order<2;++order){bool next=(round+order)%2;Pipeline p;p.uses_texture_table=table;p.trace.verbose=false;Rescaling rs;RenderArea area;area.uses_render_area=bool(scenario&2);u32 descriptor=123;const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<100000;++i){if(next)p.New(rs,area,&descriptor,false,false,bool(scenario&4));else p.Old(rs,area,&descriptor,false,false,bool(scenario&4));}p.scheduler.Execute();sink=p.trace.checksum;values[next].push_back(std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000);}
  std::cout<<"BENCH scenario="<<scenario;for(auto& v:values){std::sort(v.begin(),v.end());std::cout<<' '<<v[5];}std::cout<<'\n';
 }
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
