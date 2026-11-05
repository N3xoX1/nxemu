#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen execution of the production CMAA2 shaders and dispatch sequence.
#include "vulkan_fixture.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_edges_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_args_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_candidates_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_apply_vert_spv.h"
#include "yuzu_video_core/host_shaders/intel_cmaa2_apply_frag_spv.h"
#include "yuzu_video_core/host_shaders/fxaa_vert_spv.h"
#include "yuzu_video_core/host_shaders/fxaa_frag_spv.h"

struct CMAA {
    Context& vk;
    VkDescriptorSetLayout descriptors{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{};
    std::array<VkPipeline, 3> compute{};
    VkPipeline apply{};
    VkDescriptorSetLayout fx_descriptors{};
    VkPipelineLayout fx_layout{};
    VkPipeline fx_pipeline{};
    CMAA(Context& context) : vk(context) {
        const unsigned ids[]{1,2,3,4,5,6,7,8,12};
        const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLER};
        std::array<VkDescriptorSetLayoutBinding,9> bindings{};
        for (unsigned i=0;i<9;++i) bindings[i]={ids[i],types[i],1,
            VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount=9; dl.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(vk.device,&dl,nullptr,&descriptors));
        VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,64},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,160},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32},
            {VK_DESCRIPTOR_TYPE_SAMPLER,32},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets=32; dp.poolSizeCount=5; dp.pPoolSizes=sizes;
        Check(vkCreateDescriptorPool(vk.device,&dp,nullptr,&pool));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1; pl.pSetLayouts=&descriptors;
        Check(vkCreatePipelineLayout(vk.device,&pl,nullptr,&layout));
        const std::span<const uint32_t> kernels[]{INTEL_CMAA2_EDGES_SPV,INTEL_CMAA2_ARGS_SPV,
            INTEL_CMAA2_CANDIDATES_SPV};
        for (unsigned i=0;i<3;++i) {
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize=kernels[i].size_bytes(); sm.pCode=kernels[i].data();
            VkShaderModule module{}; Check(vkCreateShaderModule(vk.device,&sm,nullptr,&module));
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=module; cp.stage.pName="main";
            cp.layout=layout;
            Check(vkCreateComputePipelines(vk.device,VK_NULL_HANDLE,1,&cp,nullptr,&compute[i]));
            vkDestroyShaderModule(vk.device,module,nullptr);
        }
        std::swap(vk.layout,layout);
        apply=vk.Pipeline(INTEL_CMAA2_APPLY_VERT_SPV,INTEL_CMAA2_APPLY_FRAG_SPV,vk.pass16);
        std::swap(vk.layout,layout);
        VkDescriptorSetLayoutBinding fx_bindings[]{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        dl.bindingCount=2;dl.pBindings=fx_bindings;
        Check(vkCreateDescriptorSetLayout(vk.device,&dl,nullptr,&fx_descriptors));
        pl.pSetLayouts=&fx_descriptors;
        Check(vkCreatePipelineLayout(vk.device,&pl,nullptr,&fx_layout));
        std::swap(vk.layout,fx_layout);
        fx_pipeline=vk.Pipeline(FXAA_VERT_SPV,FXAA_FRAG_SPV,vk.pass16);
        std::swap(vk.layout,fx_layout);
    }
    ~CMAA() {
        vkDestroyPipeline(vk.device,fx_pipeline,nullptr);
        vkDestroyPipelineLayout(vk.device,fx_layout,nullptr);
        vkDestroyDescriptorSetLayout(vk.device,fx_descriptors,nullptr);
        vkDestroyPipeline(vk.device,apply,nullptr);
        for(auto p:compute) vkDestroyPipeline(vk.device,p,nullptr);
        vkDestroyPipelineLayout(vk.device,layout,nullptr);
        vkDestroyDescriptorPool(vk.device,pool,nullptr);
        vkDestroyDescriptorSetLayout(vk.device,descriptors,nullptr);
    }
    void Barrier(VkPipelineStageFlags src, VkPipelineStageFlags dst,
                 VkAccessFlags read, VkAccessFlags write) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask=read; b.dstAccessMask=write;
        vkCmdPipelineBarrier(vk.cmd,src,dst,0,1,&b,0,nullptr,0,nullptr);
    }
    void Run(int w,int h,int kind,std::ostream& report) {
        auto source=vk.MakeImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,vk.pass8);
        auto target=vk.MakeImage(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,vk.pass16);
        auto edges=vk.MakeImage((w+1)/2,h,VK_FORMAT_R8_UINT,VK_NULL_HANDLE,VK_IMAGE_USAGE_STORAGE_BIT);
        auto heads=vk.MakeImage((w+1)/2,(h+1)/2,VK_FORMAT_R32_UINT,VK_NULL_HANDLE,VK_IMAGE_USAGE_STORAGE_BIT);
        size_t pixels=size_t(w)*h;
        const VkDeviceSize sizes[]{std::max<size_t>(16,pixels/4*4),16,
            std::max<size_t>(16,pixels/2*8),64,16};
        std::array<Buffer,5> buffers;
        for (unsigned i=0;i<5;++i) buffers[i]=vk.MakeBuffer(sizes[i],
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(i==4?VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT:0),
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        auto staging=vk.MakeBuffer(pixels*8);
        auto pattern=Pattern(w,h);
        if(kind==1) for(size_t i=0;i<pattern.size();i+=4) {
            pattern[i]=64;pattern[i+1]=128;pattern[i+2]=192;pattern[i+3]=128;
        }
        if(kind==2) for(size_t i=0;i<pattern.size();++i)
            pattern[i]=static_cast<unsigned char>((uint32_t(i+1)*747796405u)>>24);
        std::memcpy(staging.mapped,pattern.data(),pattern.size());
        VkDescriptorSet set{};
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&descriptors;
        Check(vkAllocateDescriptorSets(vk.device,&alloc,&set));
        const unsigned imageBindings[]{1,5,8,12};
        const VkDescriptorType imageTypes[]{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLER};
        VkDescriptorImageInfo images[]{{0,edges.view,VK_IMAGE_LAYOUT_GENERAL},
            {0,heads.view,VK_IMAGE_LAYOUT_GENERAL},{0,source.view,VK_IMAGE_LAYOUT_GENERAL},
            {vk.sampler,0,VK_IMAGE_LAYOUT_UNDEFINED}};
        for(unsigned i=0;i<4;++i) {
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=imageBindings[i];wr.descriptorCount=1;
            wr.descriptorType=imageTypes[i];wr.pImageInfo=&images[i];
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        const unsigned bufferBindings[]{2,3,4,6,7};
        for(unsigned i=0;i<5;++i) {
            VkDescriptorBufferInfo bi{buffers[i].buffer,0,sizes[i]};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=bufferBindings[i];wr.descriptorCount=1;
            wr.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr.pBufferInfo=&bi;
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        vk.Begin();
        for(auto image:{source,target,edges,heads}) vk.Barrier(image,VK_IMAGE_LAYOUT_UNDEFINED);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={uint32_t(w),uint32_t(h),1};
        vkCmdCopyBufferToImage(vk.cmd,staging.buffer,source.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        vk.Barrier(source);
        vkCmdFillBuffer(vk.cmd,buffers[3].buffer,0,64,0);
        Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vk.Submit();
        std::vector<double> timings;
        std::vector<unsigned char> last;
        for(int frame=0;frame<72;++frame) {
            vk.Begin();
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdResetQueryPool(vk.cmd,vk.queries,0,2);
            vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,vk.queries,0);
            vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute[0]);
            vkCmdDispatch(vk.cmd,(w+27)/28,(h+27)/28,1);
            Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute[1]);
            vkCmdDispatch(vk.cmd,2,1,1);
            Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
            vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute[2]);
            vkCmdDispatchIndirect(vk.cmd,buffers[4].buffer,0);
            Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            vk.Barrier(target);
            VkClearValue clear{};
            VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rp.renderPass=vk.pass16;rp.framebuffer=target.fb;
            rp.renderArea.extent={uint32_t(w),uint32_t(h)};rp.clearValueCount=1;rp.pClearValues=&clear;
            vkCmdBeginRenderPass(vk.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0,0,float(w),float(h),0,1};
            VkRect2D sc{{0,0},{uint32_t(w),uint32_t(h)}};
            vkCmdSetViewport(vk.cmd,0,1,&vp);vkCmdSetScissor(vk.cmd,0,1,&sc);
            vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,apply);
            vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
            vkCmdDraw(vk.cmd,3,1,0,0);vkCmdEndRenderPass(vk.cmd);
            vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,vk.queries,1);
            vk.Barrier(target);
            vkCmdCopyImageToBuffer(vk.cmd,target.image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&copy);
            vk.Submit();
            uint64_t ticks[2];Check(vkGetQueryPoolResults(vk.device,vk.queries,0,2,sizeof(ticks),ticks,
                sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(frame>=12) timings.push_back((ticks[1]-ticks[0])*vk.properties.limits.timestampPeriod/1e6);
            std::vector<unsigned char> current(pixels*8);
            std::memcpy(current.data(),staging.mapped,current.size());
            // Dense noise can exhaust Intel's bounded atomic append lists; the
            // subset retained then depends on thread scheduling. Require exact
            // stability for normal and constant patterns, not overflow stress.
            if(frame>0 && kind!=2 && current!=last) throw std::runtime_error("CMAA2 changed across identical frames");
            last=std::move(current);
        }
        // IEEE half decode; ensure finite RGB and preserved alpha on every pixel.
        auto half=[](uint16_t v) { return (v&0x8000?-1.f:1.f)*
            ((v&0x7c00)?std::ldexp(1.f+float(v&1023)/1024.f,int((v>>10)&31)-15):std::ldexp(float(v&1023),-24)); };
        const auto* result=reinterpret_cast<const uint16_t*>(last.data());
        unsigned changed=0;
        for(size_t i=0;i<pixels;++i) {
            for(unsigned k=0;k<4;++k) {
                float value=half(result[i*4+k]);
                if((result[i*4+k]&0x7c00)==0x7c00) throw std::runtime_error("Non-finite output");
                if(k==3 && std::abs(value-pattern[i*4+3]/255.f)>.001f) throw std::runtime_error("Alpha changed");
                if(k<3 && std::abs(value-pattern[i*4+k]/255.f)>.01f) ++changed;
            }
        }
        if(kind==1 && changed) throw std::runtime_error("Constant image changed");
        if(kind==0 && pixels>1 && !changed) throw std::runtime_error("AA made no changes to the edge pattern");
        report<<"CMAA2,"<<w<<','<<h<<','<<kind<<','<<Percentile(timings,.5)<<','<<Percentile(timings,.95)<<','<<changed<<'\n';
        std::cout<<w<<'x'<<h<<" kind="<<kind<<" median="<<Percentile(timings,.5)<<" ms changed="<<changed<<'\n';
        // Same source, destination format and sample count, production FXAA shaders.
        alloc.pSetLayouts=&fx_descriptors;
        Check(vkAllocateDescriptorSets(vk.device,&alloc,&set));
        VkDescriptorImageInfo fx_input{vk.sampler,source.view,VK_IMAGE_LAYOUT_GENERAL};
        for(unsigned binding=0;binding<2;++binding) {
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet=set;wr.dstBinding=binding;wr.descriptorCount=1;
            wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&fx_input;
            vkUpdateDescriptorSets(vk.device,1,&wr,0,nullptr);
        }
        timings.clear();
        for(int frame=0;frame<72;++frame) {
            vk.Begin();vk.Barrier(target);
            vkCmdResetQueryPool(vk.cmd,vk.queries,0,2);
            vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,vk.queries,0);
            VkClearValue clear{};
            VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rp.renderPass=vk.pass16;rp.framebuffer=target.fb;
            rp.renderArea.extent={uint32_t(w),uint32_t(h)};rp.clearValueCount=1;rp.pClearValues=&clear;
            vkCmdBeginRenderPass(vk.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0,0,float(w),float(h),0,1};
            VkRect2D sc{{0,0},{uint32_t(w),uint32_t(h)}};
            vkCmdSetViewport(vk.cmd,0,1,&vp);vkCmdSetScissor(vk.cmd,0,1,&sc);
            vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,fx_pipeline);
            vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,fx_layout,0,1,&set,0,nullptr);
            vkCmdDraw(vk.cmd,3,1,0,0);vkCmdEndRenderPass(vk.cmd);
            vkCmdWriteTimestamp(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,vk.queries,1);
            vk.Submit();
            uint64_t ticks[2];Check(vkGetQueryPoolResults(vk.device,vk.queries,0,2,sizeof(ticks),ticks,
                sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(frame>=12) timings.push_back((ticks[1]-ticks[0])*vk.properties.limits.timestampPeriod/1e6);
        }
        report<<"FXAA,"<<w<<','<<h<<','<<kind<<','<<Percentile(timings,.5)<<','<<Percentile(timings,.95)<<",0\n";
        vk.Free(staging);for(auto& b:buffers) vk.Free(b);
        for(auto image:{source,target,edges,heads}) vk.Free(image);
    }
};
