// SPDX-License-Identifier: GPL-2.0-or-later
// Four production AA algorithms with a shared presentation pass. No emulator FPS.
#include "cmaa2_fixture.h"
#include "yuzu_video_core/area_sampling.h"
#include "yuzu_video_core/smaa_area_tex.h"
#include "yuzu_video_core/smaa_search_tex.h"
#include "yuzu_video_core/host_shaders/smaa_edge_detection_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_edge_detection_frag_spv.h"
#include "yuzu_video_core/host_shaders/smaa_blending_weight_calculation_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_blending_weight_calculation_frag_spv.h"
#include "yuzu_video_core/host_shaders/smaa_neighborhood_blending_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_neighborhood_blending_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_catmull_rom_frag_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_edge_detection_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_edge_detection_frag_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_blending_weight_calculation_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_blending_weight_calculation_frag_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_neighborhood_blending_vert_spv.h"
#include "yuzu_video_core/host_shaders/smaa_high_neighborhood_blending_frag_spv.h"
#include "smaa_medium_edge_detection_vert_spv.h"
#include "smaa_medium_edge_detection_frag_spv.h"
#include "smaa_medium_blending_weight_calculation_vert_spv.h"
#include "smaa_medium_blending_weight_calculation_frag_spv.h"
#include "smaa_medium_neighborhood_blending_vert_spv.h"
#include "smaa_medium_neighborhood_blending_frag_spv.h"
#include "smaa_zero_ultra_edge_detection_frag_spv.h"
#include "smaa_zero_high_edge_detection_frag_spv.h"
#include "smaa_zero_medium_edge_detection_frag_spv.h"
#include <bit>
#include <memory>

struct Graphics {
    Context& vk;
    VkDescriptorSetLayout descriptors{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    Graphics(Context& context, unsigned count, std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment, VkRenderPass pass) : vk(context) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);
        for(unsigned i=0;i<count;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount=count;dl.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(vk.device,&dl,nullptr,&descriptors));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count*32};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets=32;dp.poolSizeCount=1;dp.pPoolSizes=&size;
        Check(vkCreateDescriptorPool(vk.device,&dp,nullptr,&pool));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&descriptors;
        Check(vkCreatePipelineLayout(vk.device,&pl,nullptr,&layout));
        std::swap(vk.layout,layout);pipeline=vk.Pipeline(vertex,fragment,pass);std::swap(vk.layout,layout);
    }
    VkDescriptorSet Set(std::initializer_list<VkImageView> views) {
        VkDescriptorSet set{};
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&descriptors;
        Check(vkAllocateDescriptorSets(vk.device,&alloc,&set));
        unsigned binding=0;
        for(auto view:views) {
            VkDescriptorImageInfo image{vk.sampler,view,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=binding++;wr.descriptorCount=1;
            wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&image;
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        return set;
    }
    ~Graphics() {
        vkDestroyPipeline(vk.device,pipeline,nullptr);vkDestroyPipelineLayout(vk.device,layout,nullptr);
        vkDestroyDescriptorPool(vk.device,pool,nullptr);vkDestroyDescriptorSetLayout(vk.device,descriptors,nullptr);
    }
};

void Draw(Context& vk,const Image& target,VkRenderPass pass,VkPipeline pipeline,
          VkPipelineLayout layout,VkDescriptorSet set) {
    vk.Barrier(target);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    const VkClearValue clear{};
    rp.renderPass=pass;rp.framebuffer=target.fb;rp.renderArea.extent={uint32_t(target.w),uint32_t(target.h)};
    rp.clearValueCount=1;rp.pClearValues=&clear;
    vkCmdBeginRenderPass(vk.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0,0,float(target.w),float(target.h),0,1};
    VkRect2D sc{{0,0},{uint32_t(target.w),uint32_t(target.h)}};
    vkCmdSetViewport(vk.cmd,0,1,&vp);vkCmdSetScissor(vk.cmd,0,1,&sc);
    vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    vkCmdDraw(vk.cmd,3,1,0,0);vkCmdEndRenderPass(vk.cmd);
    vk.Barrier(target);
}

void Upload(Context& vk, const Image& image, const void* bytes, size_t size) {
    auto staging=vk.MakeBuffer(size);std::memcpy(staging.mapped,bytes,size);
    vk.Begin();vk.Barrier(image,VK_IMAGE_LAYOUT_UNDEFINED);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={uint32_t(image.w),uint32_t(image.h),1};
    vkCmdCopyBufferToImage(vk.cmd,staging.buffer,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    vk.Barrier(image);vk.Submit();vk.Free(staging);
}

std::vector<unsigned char> Download8(Context& vk,const Image& image) {
    const size_t size=size_t(image.w)*image.h*4;
    auto staging=vk.MakeBuffer(size);vk.Begin();vk.Barrier(image);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={uint32_t(image.w),uint32_t(image.h),1};
    vkCmdCopyImageToBuffer(vk.cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    vk.Submit();std::vector<unsigned char> result(size);
    std::memcpy(result.data(),staging.mapped,size);vk.Free(staging);return result;
}

struct CMAAImages {
    Context& vk;
    CMAA& aa;
    Image edges,heads;
    std::array<Buffer,5> buffers;
    VkDescriptorSet set{};
    CMAAImages(Context& context,CMAA& backend,int w,int h,const Image& source) : vk(context),aa(backend) {
        edges=vk.MakeImage((w+1)/2,h,VK_FORMAT_R8_UINT,VK_NULL_HANDLE,VK_IMAGE_USAGE_STORAGE_BIT);
        heads=vk.MakeImage((w+1)/2,(h+1)/2,VK_FORMAT_R32_UINT,VK_NULL_HANDLE,VK_IMAGE_USAGE_STORAGE_BIT);
        const size_t pixels=size_t(w)*h;
        const VkDeviceSize sizes[]{std::max<size_t>(16,pixels/4*4),16,std::max<size_t>(16,pixels/2*8),64,16};
        for(unsigned i=0;i<5;++i) buffers[i]=vk.MakeBuffer(sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
            (i==4?VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT:0),VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=aa.pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&aa.descriptors;
        Check(vkAllocateDescriptorSets(vk.device,&alloc,&set));
        const unsigned ids[]{1,5,8,12};
        const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLER};
        VkDescriptorImageInfo images[]{{0,edges.view,VK_IMAGE_LAYOUT_GENERAL},{0,heads.view,VK_IMAGE_LAYOUT_GENERAL},
            {0,source.view,VK_IMAGE_LAYOUT_GENERAL},{vk.sampler,0,VK_IMAGE_LAYOUT_UNDEFINED}};
        for(unsigned i=0;i<4;++i) {
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=ids[i];wr.descriptorCount=1;wr.descriptorType=types[i];wr.pImageInfo=&images[i];
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        const unsigned bufferIds[]{2,3,4,6,7};
        for(unsigned i=0;i<5;++i) {
            VkDescriptorBufferInfo bi{buffers[i].buffer,0,sizes[i]};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=bufferIds[i];wr.descriptorCount=1;
            wr.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr.pBufferInfo=&bi;
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        vk.Begin();vk.Barrier(edges,VK_IMAGE_LAYOUT_UNDEFINED);vk.Barrier(heads,VK_IMAGE_LAYOUT_UNDEFINED);
        vkCmdFillBuffer(vk.cmd,buffers[3].buffer,0,64,0);
        aa.Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vk.Submit();
    }
    void Record(const Image& output,VkRenderPass pass) {
        aa.Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,aa.layout,0,1,&set,0,nullptr);
        vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,aa.compute[0]);
        vkCmdDispatch(vk.cmd,(output.w+27)/28,(output.h+27)/28,1);
        aa.Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,aa.compute[1]);vkCmdDispatch(vk.cmd,2,1,1);
        aa.Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,aa.compute[2]);
        vkCmdDispatchIndirect(vk.cmd,buffers[4].buffer,0);
        aa.Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        Draw(vk,output,pass,aa.apply,aa.layout,set);
    }
    ~CMAAImages() { for(auto& b:buffers)vk.Free(b);vk.Free(edges);vk.Free(heads); }
};

int main(int argc,char** argv) try {
    if(argc!=2 && argc!=3) throw std::runtime_error("Usage: nxemu-aa-vulkan-benchmark OUTPUT_DIRECTORY [quick|high|medium|compact|compact8|PRESET-compact|PRESET-zero|PRESET-fused]");
    fs::path out(argv[1]);fs::create_directories(out);
    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,true);
    const std::string variant=argc==3?argv[2]:"reference";
    const bool quick=variant=="quick";
    const bool compact=variant=="compact" || variant=="compact8" || variant=="high-compact" || variant=="medium-compact";
    const bool compact8=variant=="compact8";
    const bool fused=variant=="ultra-fused" || variant=="high-fused" || variant=="medium-fused";
    const bool zero=fused || variant=="ultra-zero" || variant=="high-zero" || variant=="medium-zero";
    const bool high=variant=="high" || variant=="high-compact" || variant=="high-zero" || variant=="high-fused";
    const bool medium=variant=="medium" || variant=="medium-compact" || variant=="medium-zero" || variant=="medium-fused";
    if(!quick && !compact && !high && !medium && !zero && variant!="reference")throw std::runtime_error("Unknown benchmark variant");
    std::cout<<vk.properties.deviceName<<std::endl;
    std::ofstream metadata(out/"device.txt");metadata<<vk.properties.deviceName<<"\nvalidation="
        <<(std::getenv("NXEMU_FSR_VALIDATION")?1:0)<<"\ntimestamp_period_ns="<<vk.properties.limits.timestampPeriod
        <<"\nvariant="<<variant<<'\n';
    const auto pass16=vk.Pass(VK_FORMAT_R16G16B16A16_SFLOAT,VK_ATTACHMENT_LOAD_OP_LOAD);
    const auto edgeFormat=compact?VK_FORMAT_R8G8_UNORM:VK_FORMAT_R16G16_SFLOAT;
    const auto blendFormat=compact8?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R16G16B16A16_SFLOAT;
    // Production now writes explicit zero edges and never needs old attachments.
    const auto passEdge=vk.Pass(edgeFormat,compact?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    const auto passBlend=vk.Pass(blendFormat,VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    const auto passOutput=vk.Pass(VK_FORMAT_R16G16B16A16_SFLOAT,VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    const auto passCmaa=vk.Pass(VK_FORMAT_R16G16B16A16_SFLOAT,VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    {
    CMAA aa(vk);
    const std::span<const uint32_t> ultraV[]{SMAA_EDGE_DETECTION_VERT_SPV,SMAA_BLENDING_WEIGHT_CALCULATION_VERT_SPV,SMAA_NEIGHBORHOOD_BLENDING_VERT_SPV};
    const std::span<const uint32_t> ultraF[]{SMAA_EDGE_DETECTION_FRAG_SPV,SMAA_BLENDING_WEIGHT_CALCULATION_FRAG_SPV,SMAA_NEIGHBORHOOD_BLENDING_FRAG_SPV};
    const std::span<const uint32_t> highV[]{SMAA_HIGH_EDGE_DETECTION_VERT_SPV,SMAA_HIGH_BLENDING_WEIGHT_CALCULATION_VERT_SPV,SMAA_HIGH_NEIGHBORHOOD_BLENDING_VERT_SPV};
    const std::span<const uint32_t> highF[]{SMAA_HIGH_EDGE_DETECTION_FRAG_SPV,SMAA_HIGH_BLENDING_WEIGHT_CALCULATION_FRAG_SPV,SMAA_HIGH_NEIGHBORHOOD_BLENDING_FRAG_SPV};
    const std::span<const uint32_t> mediumV[]{SMAA_MEDIUM_EDGE_DETECTION_VERT_SPV,SMAA_MEDIUM_BLENDING_WEIGHT_CALCULATION_VERT_SPV,SMAA_MEDIUM_NEIGHBORHOOD_BLENDING_VERT_SPV};
    const std::span<const uint32_t> mediumF[]{SMAA_MEDIUM_EDGE_DETECTION_FRAG_SPV,SMAA_MEDIUM_BLENDING_WEIGHT_CALCULATION_FRAG_SPV,SMAA_MEDIUM_NEIGHBORHOOD_BLENDING_FRAG_SPV};
    const auto* v=high?highV:medium?mediumV:ultraV;
    const auto* f=high?highF:medium?mediumF:ultraF;
    const std::span<const uint32_t> zeroF=high?std::span<const uint32_t>{SMAA_ZERO_HIGH_EDGE_DETECTION_FRAG_SPV}:
        medium?std::span<const uint32_t>{SMAA_ZERO_MEDIUM_EDGE_DETECTION_FRAG_SPV}:std::span<const uint32_t>{SMAA_ZERO_ULTRA_EDGE_DETECTION_FRAG_SPV};
    Graphics edge(vk,1,v[0],zero?zeroF:f[0],passEdge);
    Graphics weights(vk,3,v[1],f[1],passBlend);
    Graphics neighborhood(vk,2,v[2],f[2],passOutput);
    Graphics neighborhoodFinal(vk,2,v[2],f[2],vk.pass8);
    const std::array<VkPipeline,3> present{
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV,VULKAN_PRESENT_FRAG_SPV,vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV,PRESENT_CATMULL_ROM_FRAG_SPV,vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV,PRESENT_AREA_FRAG_SPV,vk.pass8)};
    auto area=vk.MakeImage(AREATEX_WIDTH,AREATEX_HEIGHT,VK_FORMAT_R8G8_UNORM,VK_NULL_HANDLE);
    auto search=vk.MakeImage(SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,VK_FORMAT_R8_UNORM,VK_NULL_HANDLE);
    Upload(vk,area,areaTexBytes,sizeof(areaTexBytes));Upload(vk,search,searchTexBytes,sizeof(searchTexBytes));
    struct Case {const char* name;int w,h,ow,oh,filter,kind;};
    std::vector<Case> cases{{"720p-native",1280,720,1280,720,0,0},
        {"1080p-native",1920,1080,1920,1080,0,0},{"4k-native",3840,2160,3840,2160,0,0},
        {"720p-1080p-catmull",1280,720,1920,1080,1,0},{"4k-1080p-area",3840,2160,1920,1080,2,0},
        {"1080p-noise",1920,1080,1920,1080,0,2},{"4k-noise",3840,2160,3840,2160,0,2},
        {"1080p-flat",1920,1080,1920,1080,0,1}};
    if(quick) cases={{"odd-smoke",63,47,91,69,1,0}};
    std::ofstream raw(out/"samples.csv"),summary(out/"summary.csv"),checks(out/"checks.csv"),history(out/"history.csv");
    history<<"case,history_dependent_rgb_channels,max_rgb_difference\n";
    raw<<"case,mode,round,frame,aa_ms,present_ms,total_ms\n";
    summary<<"case,mode,aa_median_ms,present_median_ms,total_median_ms,total_p95_ms,round_min_ms,round_max_ms\n";
    checks<<"case,mode,changed_rgb_channels,max_rgb_difference\n";
    const char* names[]{"None","FXAA","SMAA","CMAA2"};
    for(const auto& c:cases) {
        auto source=vk.MakeImage(c.w,c.h,VK_FORMAT_R8G8B8A8_UNORM,vk.pass8);
        auto pattern=Pattern(c.w,c.h);
        if(c.kind==1) for(size_t i=0;i<pattern.size();++i) pattern[i]=128;
        if(c.kind==2) for(size_t i=0;i<pattern.size();++i) pattern[i]=static_cast<unsigned char>((uint32_t(i+1)*747796405u)>>24);
        Upload(vk,source,pattern.data(),pattern.size());
        std::array<Image,3> output;
        for(auto& im:output)im=vk.MakeImage(c.w,c.h,VK_FORMAT_R16G16B16A16_SFLOAT,pass16);
        auto edges=vk.MakeImage(c.w,c.h,edgeFormat,passEdge);
        auto blend=vk.MakeImage(c.w,c.h,blendFormat,passBlend);
        auto final=vk.MakeImage(c.ow,c.oh,VK_FORMAT_R8G8B8A8_UNORM,vk.pass8);
        vk.Begin();
        const VkClearColorValue clear{};const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        for(auto im:{output[0],output[1],output[2],edges,blend,final}) {
            vk.Barrier(im,VK_IMAGE_LAYOUT_UNDEFINED);
            vkCmdClearColorImage(vk.cmd,im.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);vk.Barrier(im);
        }
        vk.Submit();
        const auto ed=edge.Set({source.view}),we=weights.Set({edges.view,area.view,search.view}),
            nb=neighborhood.Set({source.view,blend.view});
        VkDescriptorSet fx{};VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=aa.pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&aa.fx_descriptors;
        Check(vkAllocateDescriptorSets(vk.device,&alloc,&fx));
        for(unsigned binding=0;binding<2;++binding) {
            VkDescriptorImageInfo info{vk.sampler,source.view,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=fx;wr.dstBinding=binding;
            wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&info;
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        {
        CMAAImages images(vk,aa,c.w,c.h,source);
        Vulkan::PresentPushConstants pc{};pc.transform=Vulkan::MakeOrthographicTransform(float(c.ow),float(c.oh));
        pc.vertices={Vulkan::ScreenRectVertex(0,0,0,0),Vulkan::ScreenRectVertex(float(c.ow),0,1,0),
            Vulkan::ScreenRectVertex(0,float(c.oh),0,1),Vulkan::ScreenRectVertex(float(c.ow),float(c.oh),1,1)};
        if(c.filter==2) {
            const float values[]{float(c.w)/c.ow,float(c.h)/c.oh,1.f/c.w,1.f/c.h};
            for(unsigned i=0;i<4;++i)pc.filter_constants[i]=std::bit_cast<uint32_t>(values[i]);
            pc.filter_constants[4]=VideoCore::AreaSamplingGrid(c.w,c.h,0,0,1,1,c.ow,c.oh);
        }
        std::array<std::vector<double>,4> aaSamples,presentSamples,totalSamples,roundMedians;
        const bool fuseNative=fused && c.w==c.ow && c.h==c.oh && c.filter==0;
        const auto renderAA=[&](int mode) {
            if(mode==1) Draw(vk,output[0],pass16,aa.fx_pipeline,aa.fx_layout,fx);
            if(mode==2) {
                Draw(vk,edges,passEdge,edge.pipeline,edge.layout,ed);
                Draw(vk,blend,passBlend,weights.pipeline,weights.layout,we);
                if(fuseNative)Draw(vk,final,vk.pass8,neighborhoodFinal.pipeline,neighborhoodFinal.layout,nb);
                else Draw(vk,output[1],passOutput,neighborhood.pipeline,neighborhood.layout,nb);
            }
            if(mode==3)images.Record(output[2],passCmaa);
        };
        for(int round=0;round<(quick?1:3);++round) {
            std::array<std::vector<double>,4> roundSamples;
            for(int frame=-24;frame<(quick?8:96);++frame) for(int order=0;order<4;++order) {
                const int mode=(order+frame+24+round)%4;
                vk.Begin();vk.Barrier(source);
                vkCmdResetQueryPool(vk.cmd,vk.queries,0,3);
                vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,vk.queries,0);
                renderAA(mode);
                vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,vk.queries,1);
                if(!(fuseNative && mode==2))vk.Draw(final,mode?output[mode-1]:source,vk.pass8,present[c.filter],&pc,sizeof(pc),4);
                vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,vk.queries,2);
                vk.Submit();
                uint64_t ticks[3];Check(vkGetQueryPoolResults(vk.device,vk.queries,0,3,sizeof(ticks),ticks,
                    sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
                const uint64_t mask=vk.bits==64?~uint64_t{0}:(uint64_t{1}<<vk.bits)-1;
                const double scale=vk.properties.limits.timestampPeriod/1e6;
                if(frame>=0) {
                    const double a=((ticks[1]-ticks[0])&mask)*scale,p=((ticks[2]-ticks[1])&mask)*scale,
                        total=((ticks[2]-ticks[0])&mask)*scale;
                    aaSamples[mode].push_back(a);presentSamples[mode].push_back(p);totalSamples[mode].push_back(total);
                    roundSamples[mode].push_back(total);raw<<c.name<<','<<names[mode]<<','<<round<<','<<frame<<','<<a<<','<<p<<','<<total<<'\n';
                }
            }
            for(unsigned m=0;m<4;++m)roundMedians[m].push_back(Percentile(roundSamples[m],.5));
        }
        for(unsigned m=0;m<4;++m) {
            summary<<c.name<<','<<names[m]<<','<<Percentile(aaSamples[m],.5)<<','<<Percentile(presentSamples[m],.5)<<','
                <<Percentile(totalSamples[m],.5)<<','<<Percentile(totalSamples[m],.95)<<','
                <<*std::min_element(roundMedians[m].begin(),roundMedians[m].end())<<','
                <<*std::max_element(roundMedians[m].begin(),roundMedians[m].end())<<'\n';
            std::cout<<c.name<<' '<<names[m]<<" AA="<<Percentile(aaSamples[m],.5)
                <<" present="<<Percentile(presentSamples[m],.5)<<" total="<<Percentile(totalSamples[m],.5)<<" ms"<<std::endl;
        }
        // Readbacks are outside all timed ranges. Check that the paths actually
        // render, preserve flat RGB, and that native None exactly presents input.
        auto staging=vk.MakeBuffer(size_t(c.ow)*c.oh*4);
        std::vector<unsigned char> baseline;
        for(unsigned mode=0;mode<4;++mode) {
            vk.Begin();vk.Barrier(source);renderAA(mode);
            if(!(fuseNative && mode==2))vk.Draw(final,mode?output[mode-1]:source,vk.pass8,present[c.filter],&pc,sizeof(pc),4);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent={uint32_t(c.ow),uint32_t(c.oh),1};
            vkCmdCopyImageToBuffer(vk.cmd,final.image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&copy);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
            vk.Submit();
            auto* data=static_cast<const unsigned char*>(staging.mapped);
            std::vector<unsigned char> pixels(data,data+size_t(c.ow)*c.oh*4);
            if(!mode) {
                baseline=pixels;
                if(c.w==c.ow && c.h==c.oh && pixels!=pattern)throw std::runtime_error("Native None does not reproduce input");
            }
            unsigned changed=0,maximum=0;
            for(size_t i=0;i<pixels.size();++i)if(i%4!=3) {
                const unsigned difference=unsigned(std::abs(int(pixels[i])-int(baseline[i])));
                changed+=difference>1;maximum=std::max(maximum,difference);
            }
            checks<<c.name<<','<<names[mode]<<','<<changed<<','<<maximum<<'\n';
            if(c.kind==1 && maximum>1)throw std::runtime_error("AA changed flat RGB");
            if(c.kind==0 && mode && !changed)throw std::runtime_error("AA path did not change edge pattern");
            if(c.kind==0)Save(out/(std::string(c.name)+"-"+names[mode]+".ppm"),pixels,c.ow,c.oh);
        }
        vk.Free(staging);
        if(std::string(c.name)=="1080p-native" || quick) {
            auto shifted=pattern;
            std::rotate(shifted.begin(),shifted.begin()+4*17,shifted.end());
            Upload(vk,source,shifted.data(),shifted.size());
            const auto renderSmaa=[&] {
                vk.Begin();renderAA(2);if(!fuseNative)vk.Draw(final,output[1],vk.pass8,present[c.filter],&pc,sizeof(pc),4);vk.Submit();
                return Download8(vk,final);
            };
            const auto afterPrevious=renderSmaa();
            vk.Begin();vk.Barrier(edges);
            vkCmdClearColorImage(vk.cmd,edges.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
            vk.Barrier(edges);vk.Submit();
            const auto afterClear=renderSmaa();
            unsigned changed=0,maximum=0;
            for(size_t i=0;i<afterClear.size();++i)if(i%4!=3) {
                const unsigned d=unsigned(std::abs(int(afterPrevious[i])-int(afterClear[i])));
                changed+=d!=0;maximum=std::max(maximum,d);
            }
            history<<c.name<<','<<changed<<','<<maximum<<'\n';
            std::cout<<"SMAA history dependence: "<<changed<<" RGB channels, max="<<maximum<<std::endl;
            if(changed)throw std::runtime_error("SMAA retained previous contours");
        }
        }
        vk.Free(source);for(auto& im:output)vk.Free(im);vk.Free(edges);vk.Free(blend);vk.Free(final);
    }
    vk.Free(area);vk.Free(search);for(auto p:present)vkDestroyPipeline(vk.device,p,nullptr);
    }
    vkDestroyRenderPass(vk.device,pass16,nullptr);vkDestroyRenderPass(vk.device,passEdge,nullptr);
    vkDestroyRenderPass(vk.device,passCmaa,nullptr);
    vkDestroyRenderPass(vk.device,passBlend,nullptr);vkDestroyRenderPass(vk.device,passOutput,nullptr);
    if(validation_errors)throw std::runtime_error("Vulkan validation errors: "+std::to_string(validation_errors));
    std::cout<<"PASS; validation="<<(std::getenv("NXEMU_FSR_VALIDATION")?"enabled":"disabled")<<std::endl;
} catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
