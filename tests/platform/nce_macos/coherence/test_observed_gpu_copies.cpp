// Actual MoltenVK transfers using the production sparse-copy plan.
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include "yuzu_video_core/texture_cache/observed_readback_plan.h"
#include "yuzu_video_core/texture_cache/readback_page_tracker.h"
#include "yuzu_video_core/textures/swizzle_range.h"
using namespace VideoCommon;
void check(bool b,const char* m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
void vkcheck(VkResult r){if(r!=VK_SUCCESS){fprintf(stderr,"Vulkan error %d\n",r);exit(2);}}
int main(){
 VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.pApplicationName="NCE sparse readback validation";ai.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&ai;
 VkInstance instance;vkcheck(vkCreateInstance(&ic,nullptr,&instance));
 u32 count=0;vkcheck(vkEnumeratePhysicalDevices(instance,&count,nullptr));check(count>0,"GPU available");std::vector<VkPhysicalDevice> devices(count);vkcheck(vkEnumeratePhysicalDevices(instance,&count,devices.data()));auto physical=devices.front();
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());u32 family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;check(family<count,"graphics/transfer queue");
 float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
 VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
 const char* portability="VK_KHR_portability_subset";dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=&portability;
 VkDevice device;vkcheck(vkCreateDevice(physical,&dc,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties properties;vkGetPhysicalDeviceMemoryProperties(physical,&properties);
 auto allocate=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags){u32 type=0;while(type<properties.memoryTypeCount&&(!(req.memoryTypeBits&(1u<<type))||(properties.memoryTypes[type].propertyFlags&flags)!=flags))++type;check(type<properties.memoryTypeCount,"memory type");VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;VkDeviceMemory m;vkcheck(vkAllocateMemory(device,&a,nullptr,&m));return m;};
 VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pci.queueFamilyIndex=family;VkCommandPool pool;vkcheck(vkCreateCommandPool(device,&pci,nullptr,&pool));
 VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;vkcheck(vkAllocateCommandBuffers(device,&ca,&cmd));
 auto begin=[&]{vkcheck(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkcheck(vkBeginCommandBuffer(cmd,&b));};
 auto submit=[&]{vkcheck(vkEndCommandBuffer(cmd));VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;vkcheck(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE));vkcheck(vkQueueWaitIdle(queue));};
 std::mt19937 rng(130);unsigned cases=0,partial_cases=0;
 for(u32 width:{17U,65U,512U,513U})for(u32 height:{9U,65U,512U})for(u32 bh:{0U,1U,4U}){
  const u64 bytes=u64(width)*height*4;VkBuffer buffers[3];VkDeviceMemory memory[3];void* mapped[3];
  for(unsigned n=0;n<3;++n){VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=bytes;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;vkcheck(vkCreateBuffer(device,&b,nullptr,&buffers[n]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[n],&req);memory[n]=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);vkcheck(vkBindBufferMemory(device,buffers[n],memory[n],0));vkcheck(vkMapMemory(device,memory[n],0,VK_WHOLE_SIZE,0,&mapped[n]));memset(mapped[n],n?0x5c:0,bytes);}
  auto* input=static_cast<u8*>(mapped[0]);for(u64 i=0;i<bytes;++i)input[i]=rng();
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;image_info.format=VK_FORMAT_R8G8B8A8_UNORM;image_info.extent={width,height,1};image_info.mipLevels=1;image_info.arrayLayers=1;image_info.samples=VK_SAMPLE_COUNT_1_BIT;image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage image;vkcheck(vkCreateImage(device,&image_info,nullptr,&image));VkMemoryRequirements image_req;vkGetImageMemoryRequirements(device,image,&image_req);auto image_memory=allocate(image_req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vkcheck(vkBindImageMemory(device,image,image_memory,0));
  auto barrier=[&](VkImageLayout old_layout,VkImageLayout layout,VkAccessFlags src,VkAccessFlags dst){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=old_layout;b.newLayout=layout;b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);};
  VkBufferImageCopy full{};full.bufferRowLength=width;full.bufferImageHeight=height;full.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};full.imageExtent={width,height,1};
  begin();barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);vkCmdCopyBufferToImage(cmd,buffers[0],image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&full);barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[1],1,&full);submit();check(memcmp(input,mapped[1],bytes)==0,"full GPU reference transfer");
  const u64 tiled_size=((u64(height)+(8ULL<<bh)-1)/(8ULL<<bh))*((u64(width)*4+63)/64)*(512ULL<<bh);
  ReadbackPageTracker tracker;tracker.Observe(0x100003,1);if(width>65)tracker.Observe(0x100003+tiled_size/2,1);
  u64 serial=tracker.Serial();std::vector<std::pair<u64,u64>> ranges;tracker.ForEachRange(0x100003,tiled_size,serial,[&](u64 a,u64 n){ranges.emplace_back(a,n);});
  auto plan=PlanObservedReadback(width*4,height,bh,ranges);std::vector<VkBufferImageCopy> regions;
  if(plan){++partial_cases;for(auto r:*plan){auto copy=full;copy.bufferOffset=(u64(r.y)*width+r.x)*4;copy.imageOffset={s32(r.x),s32(r.y),0};copy.imageExtent={r.width,r.height,1};regions.push_back(copy);}}
  else regions.push_back(full);
  begin();if(!regions.empty())vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[2],u32(regions.size()),regions.data());
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);submit();
  // Compare publication from the actual partial GPU buffer to a full GPU copy,
  // including late page observations and newer synchronous alias bytes.
  tracker.Observe(0x100003+tiled_size-1,1);tracker.MarkSynchronized(0x100003+7,19);
  std::vector<u8> result(tiled_size,0xa7),expected=result;
  tracker.ForEachRange(0x100003,tiled_size,serial,[&](u64 a,u64 n){Tegra::Texture::Swizzle2DRange(std::span(result).subspan(a,n),{static_cast<u8*>(mapped[2]),size_t(bytes)},width*4,height,bh,a);Tegra::Texture::Swizzle2DRange(std::span(expected).subspan(a,n),{static_cast<u8*>(mapped[1]),size_t(bytes)},width*4,height,bh,a);});
  check(result==expected,"actual partial GPU transfers match full GPU publication");++cases;
  vkDestroyImage(device,image,nullptr);vkFreeMemory(device,image_memory,nullptr);for(unsigned n=0;n<3;++n){vkUnmapMemory(device,memory[n]);vkDestroyBuffer(device,buffers[n],nullptr);vkFreeMemory(device,memory[n],nullptr);}
 }
 vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
 printf("PASS actual MoltenVK GPU: %u layouts, %u sparse plans; full-reference upload/download, sparse strided copies, late observation and newer alias publication\n",cases,partial_cases);
}
