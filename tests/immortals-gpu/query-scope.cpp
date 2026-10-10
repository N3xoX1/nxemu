// Real GPU regression: query endpoints across render passes, plus presentation uploads.
#define main PriorTestMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
namespace vk {
struct CommandBuffer {
    VkCommandBuffer handle;
    void PipelineBarrier(VkPipelineStageFlags src, VkPipelineStageFlags dst,
                         VkDependencyFlags flags, const VkImageMemoryBarrier& barrier) {
        vkCmdPipelineBarrier(handle, src, dst, flags, 0, nullptr, 0, nullptr, 1, &barrier);
    }
};
}
#include "presentation-transition.inc"
VkShaderModule LoadShader(VkDevice device, const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing graphics shader");
    const auto size = file.tellg();
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), size);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4; ci.pCode = words.data();
    VkShaderModule result{}; Check(vkCreateShaderModule(device, &ci, nullptr, &result));
    return result;
}
int main(int argc, char** argv) try {
    if (argc != 5) return 2;
    Fixture f(argv[1]);
    const bool fixed = std::string(argv[4]) == "fixed";
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {4,4,1}; ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image{}; Check(vkCreateImage(f.device, &ici, nullptr, &image));
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(f.device, image, &req);
    uint32_t type = 0; while (!(req.memoryTypeBits & (1U << type))) ++type;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    VkDeviceMemory memory{}; Check(vkAllocateMemory(f.device, &mai, nullptr, &memory));
    Check(vkBindImageMemory(f.device, image, memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ici.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{}; Check(vkCreateImageView(f.device, &vi, nullptr, &view));
    VkAttachmentDescription attachment{};
    attachment.format = ici.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1; rci.pAttachments = &attachment;
    rci.subpassCount = 1; rci.pSubpasses = &subpass; rci.dependencyCount = 1; rci.pDependencies = &dependency;
    VkRenderPass pass{}; Check(vkCreateRenderPass(f.device, &rci, nullptr, &pass));
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = pass; fbci.attachmentCount = 1; fbci.pAttachments = &view;
    fbci.width = fbci.height = 4; fbci.layers = 1;
    VkFramebuffer framebuffer{}; Check(vkCreateFramebuffer(f.device, &fbci, nullptr, &framebuffer));
    const auto vertex = LoadShader(f.device, argv[2]);
    const auto fragment = LoadShader(f.device, argv[3]);
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    for (int i=0;i<2;++i) { stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage=i==0?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module=i==0?vertex:fragment; stages[i].pName="main"; }
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1}; VkRect2D scissor{{0,0},{4,4}};
    VkPipelineViewportStateCreateInfo vci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vci.viewportCount=vci.scissorCount=1; vci.pViewports=&viewport; vci.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode=VK_POLYGON_MODE_FILL; raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo bci{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bci.attachmentCount=1; bci.pAttachments=&blend;
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{}; Check(vkCreatePipelineLayout(f.device,&lci,nullptr,&layout));
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount=2; pci.pStages=stages.data(); pci.pVertexInputState=&input;
    pci.pInputAssemblyState=&assembly; pci.pViewportState=&vci; pci.pRasterizationState=&raster;
    pci.pMultisampleState=&samples; pci.pColorBlendState=&bci; pci.layout=layout; pci.renderPass=pass;
    VkPipeline pipeline{}; Check(vkCreateGraphicsPipelines(f.device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType=VK_QUERY_TYPE_OCCLUSION; qci.queryCount=2;
    VkQueryPool queries{}; Check(vkCreateQueryPool(f.device,&qci,nullptr,&queries));
    std::fill_n(f.buffers[0].mapped,16,0xff00ff00u);
    f.Begin(); auto cb=f.commands[1]; vk::CommandBuffer wrapped{cb};
    TransitionImageLayout(wrapped,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_UNDEFINED);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={4,4,1};
    vkCmdCopyBufferToImage(cb,f.buffers[0].buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    TransitionImageLayout(wrapped,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Fixture::Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,f.buffers[2].buffer,1,&copy);
    vkCmdResetQueryPool(cb,queries,0,2);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=pass; begin.framebuffer=framebuffer; begin.renderArea={{0,0},{4,4}};
    auto draw=[&]{vkCmdBeginRenderPass(cb,&begin,VK_SUBPASS_CONTENTS_INLINE);
                  vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(cb,3,1,0,0);};
    vkCmdBeginQuery(cb,queries,0,VK_QUERY_CONTROL_PRECISE_BIT); draw();
    if(fixed)vkCmdEndRenderPass(cb);
    vkCmdEndQuery(cb,queries,0);
    if(!fixed)vkCmdEndRenderPass(cb);
    vkCmdBeginQuery(cb,queries,1,VK_QUERY_CONTROL_PRECISE_BIT);
    draw();vkCmdEndRenderPass(cb);draw();vkCmdEndRenderPass(cb);
    vkCmdEndQuery(cb,queries,1);
    Fixture::Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    f.Submit();
    std::array<uint64_t,2> results{};
    Check(vkGetQueryPoolResults(f.device,queries,0,2,sizeof(results),results.data(),8,
                               VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    bool ok=results[0]==16 && results[1]==32;
    for(int i=0;i<16;++i)ok &= f.buffers[2].mapped[i]==0xff00ff00u;
    std::cout<<"RESULT query_scope fixed="<<fixed<<" samples="<<results[0]<<","<<results[1]
             <<" upload="<<std::hex<<f.buffers[2].mapped[0]<<std::dec<<'\n';
    vkDestroyQueryPool(f.device,queries,nullptr);vkDestroyPipeline(f.device,pipeline,nullptr);
    vkDestroyPipelineLayout(f.device,layout,nullptr);vkDestroyShaderModule(f.device,vertex,nullptr);
    vkDestroyShaderModule(f.device,fragment,nullptr);vkDestroyFramebuffer(f.device,framebuffer,nullptr);
    vkDestroyRenderPass(f.device,pass,nullptr);vkDestroyImageView(f.device,view,nullptr);
    vkDestroyImage(f.device,image,nullptr);vkFreeMemory(f.device,memory,nullptr);
    return !fixed || ok ? 0 : 1;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
