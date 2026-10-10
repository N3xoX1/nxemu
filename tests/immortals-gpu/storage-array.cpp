// Execute production-emitted shared-memory -> runtime TIC image stores on the GPU.
#define main PriorTestMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
int main(int argc,char** argv) try {
 if(argc!=3)return 2;
 Fixture f(argv[1],true);
 std::array<VkImage,4> images{};
 std::array<VkDeviceMemory,4> memories{};
 std::array<VkImageView,4> views{};
 const std::array<VkFormat,4> formats{VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT,VK_FORMAT_R32_SFLOAT};
 for(size_t i=0;i<4;++i){
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType=VK_IMAGE_TYPE_3D;ci.format=formats[i];ci.extent={1,1,1};
  ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  Check(vkCreateImage(f.device,&ci,nullptr,&images[i]));
  VkMemoryRequirements req{};vkGetImageMemoryRequirements(f.device,images[i],&req);
  uint32_t type=0;while(!(req.memoryTypeBits&(1U<<type)))++type;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  Check(vkAllocateMemory(f.device,&ai,nullptr,&memories[i]));Check(vkBindImageMemory(f.device,images[i],memories[i],0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[i];vi.viewType=VK_IMAGE_VIEW_TYPE_3D;
  vi.format=formats[i];vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  Check(vkCreateImageView(f.device,&vi,nullptr,&views[i]));
 }
 std::array<VkDescriptorSetLayoutBinding,4> bindings{};
 for(uint32_t i=0;i<3;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,5,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
 bindings[3]={3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
 VkDescriptorSetLayout layout{};VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
 lci.bindingCount=4;lci.pBindings=bindings.data();Check(vkCreateDescriptorSetLayout(f.device,&lci,nullptr,&layout));
 VkPipelineLayout pipeline_layout{};VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
 plci.setLayoutCount=1;plci.pSetLayouts=&layout;Check(vkCreatePipelineLayout(f.device,&plci,nullptr,&pipeline_layout));
 VkDescriptorPool descriptor_pool{};
 const std::array<VkDescriptorPoolSize,2> ps{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,15},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
 VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=2;dpci.pPoolSizes=ps.data();
 Check(vkCreateDescriptorPool(f.device,&dpci,nullptr,&descriptor_pool));
 VkDescriptorSet set{};VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
 dsai.descriptorPool=descriptor_pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&layout;Check(vkAllocateDescriptorSets(f.device,&dsai,&set));
 std::array<VkDescriptorImageInfo,15> infos{};
 std::array<VkWriteDescriptorSet,4> writes{};
 for(uint32_t type=0;type<3;++type){
  for(uint32_t i=0;i<5;++i)infos[type*5+i]={VK_NULL_HANDLE,(i==type || (type==0 && i==3))?views[i]:VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL};
  auto& write=writes[type];write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=type;write.descriptorCount=5;
  write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&infos[type*5];
 }
 VkDescriptorBufferInfo mask_info{f.buffers[1].buffer,0,4};
 writes[3]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[3].dstSet=set;writes[3].dstBinding=3;
 writes[3].descriptorCount=1;writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[3].pBufferInfo=&mask_info;
 vkUpdateDescriptorSets(f.device,4,writes.data(),0,nullptr);
 std::ifstream file(argv[2],std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Missing production shader");
 auto size=file.tellg();std::vector<uint32_t> code(static_cast<size_t>(size)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),size);
 VkShaderModule shader{};VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=code.size()*4;smci.pCode=code.data();
 Check(vkCreateShaderModule(f.device,&smci,nullptr,&shader));
 VkPipeline pipeline{};VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.layout=pipeline_layout;
 cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cpci.stage.module=shader;cpci.stage.pName="main";
 Check(vkCreateComputePipelines(f.device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
 f.Begin();auto cb=f.commands[1];
 vkCmdFillBuffer(cb,f.buffers[1].buffer,0,4,0);
 std::array<VkImageMemoryBarrier,4> barriers{};
 for(size_t i=0;i<4;++i){auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[i];
  b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;}
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,4,barriers.data());
 VkClearColorValue clear{};
 for(size_t i=0;i<4;++i)vkCmdClearColorImage(cb,images[i],VK_IMAGE_LAYOUT_GENERAL,&clear,1,&barriers[i].subresourceRange);
 Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
 vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
 vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(cb,1,1,1);
 Fixture::Barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
 for(size_t i=0;i<4;++i){VkBufferImageCopy copy{};copy.bufferOffset=i*4;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
  vkCmdCopyImageToBuffer(cb,images[i],VK_IMAGE_LAYOUT_GENERAL,f.buffers[0].buffer,1,&copy);}
 Fixture::Barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
 Fixture::Barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
 f.Submit();bool ok=true;
 const uint32_t mask=f.buffers[1].mapped[0];ok &= mask==23;
 std::cout<<"WRITE_MASK expected=23 observed="<<mask<<" unused_valid_handle_3="<<((mask>>3)&1)<<'\n';
 const std::array<uint32_t,4> expected_values{0x3f800000u,42u,0xfffffff9u,0u};
 for(size_t i=0;i<4;++i){uint32_t expected=expected_values[i];ok&=f.buffers[0].mapped[i]==expected;
  std::cout<<"IMAGE "<<i<<" expected="<<expected<<" observed="<<f.buffers[0].mapped[i]<<'\n';}
 vkDestroyPipeline(f.device,pipeline,nullptr);vkDestroyShaderModule(f.device,shader,nullptr);
 vkDestroyPipelineLayout(f.device,pipeline_layout,nullptr);vkDestroyDescriptorPool(f.device,descriptor_pool,nullptr);vkDestroyDescriptorSetLayout(f.device,layout,nullptr);
 for(size_t i=0;i<4;++i){vkDestroyImageView(f.device,views[i],nullptr);vkDestroyImage(f.device,images[i],nullptr);vkFreeMemory(f.device,memories[i],nullptr);}
 std::cout<<(ok?"PASS":"FAIL")<<" divergent shared handles select float/uint/sint; invalid handle discards writes\n";return ok?0:1;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
