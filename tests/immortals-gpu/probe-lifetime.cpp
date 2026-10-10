// Execute the exact Windows probe body with an instrumented Vulkan dispatch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include "yuzu_common/common_types.h"
#include "yuzu_common/scope_exit.h"
namespace fmt { template<class... T> void print(FILE*,const char*,T...) {} }
struct VkDeviceRecord { VkDeviceRecord(std::string,std::vector<uint32_t>,bool) {} };
std::vector<std::string> events;
bool fail_surface=false;
HMODULE OpenVulkanLibrary(){events.push_back("open");return reinterpret_cast<HMODULE>(1);}
VkInstance CreateVulkanInstance(HMODULE,u32,bool){events.push_back("instance");return reinterpret_cast<VkInstance>(1);}
std::vector<VkPhysicalDevice> EnumeratePhysicalDevices(VkInstance,PFN_vkGetInstanceProcAddr){return {reinterpret_cast<VkPhysicalDevice>(2)};}
bool CheckBrokenCompute(VkDriverId,u32){return false;}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance,const VkAllocationCallbacks*){events.push_back("destroy_instance");}
VKAPI_ATTR VkResult VKAPI_CALL CreateSurface(VkInstance,const VkWin32SurfaceCreateInfoKHR*,const VkAllocationCallbacks*,VkSurfaceKHR* surface){
 if(fail_surface)return VK_ERROR_INITIALIZATION_FAILED;
 events.push_back("surface");*surface=reinterpret_cast<VkSurfaceKHR>(3);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance,VkSurfaceKHR,const VkAllocationCallbacks*){events.push_back("destroy_surface");}
VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice,VkPhysicalDeviceProperties* p){*p={};strcpy_s(p->deviceName,"Synthetic GPU");}
VKAPI_ATTR void VKAPI_CALL Properties2(VkPhysicalDevice,VkPhysicalDeviceProperties2* p){p->properties={};}
VKAPI_ATTR VkResult VKAPI_CALL PresentModes(VkPhysicalDevice,VkSurfaceKHR,uint32_t* count,VkPresentModeKHR*){*count=0;return VK_SUCCESS;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance,const char* name){
 const std::string_view n=name;
 if(n=="vkDestroyInstance")return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
 if(n=="vkCreateWin32SurfaceKHR")return reinterpret_cast<PFN_vkVoidFunction>(CreateSurface);
 if(n=="vkDestroySurfaceKHR")return reinterpret_cast<PFN_vkVoidFunction>(DestroySurface);
 if(n=="vkGetPhysicalDeviceProperties")return reinterpret_cast<PFN_vkVoidFunction>(Properties);
 if(n=="vkGetPhysicalDeviceProperties2")return reinterpret_cast<PFN_vkVoidFunction>(Properties2);
 if(n=="vkGetPhysicalDeviceSurfacePresentModesKHR")return reinterpret_cast<PFN_vkVoidFunction>(PresentModes);
 return nullptr;
}
FARPROC FakeGetProcAddress(HMODULE,const char*){return reinterpret_cast<FARPROC>(InstanceProc);}
BOOL FakeFreeLibrary(HMODULE){events.push_back("unload");return TRUE;}
#include PROBE_BODY
int main(){
 bool ok=true;
 for(bool fail:{false,true}){
  fail_surface=fail;events.clear();std::vector<VkDeviceRecord> records;
  PopulateVulkanRecords(records,nullptr);
  const std::vector<std::string> expected=fail?
   std::vector<std::string>{"open","instance","destroy_instance","unload"}:
   std::vector<std::string>{"open","instance","surface","destroy_surface","destroy_instance","unload"};
  const bool matched=events==expected;ok &= matched;
  std::cout<<(matched?"PASS":"FAIL")<<" Vulkan probe cleanup "<<(fail?"surface_failure":"normal")<<":";
  for(const auto& e:events)std::cout<<' '<<e;std::cout<<'\n';
 }
 return ok?0:1;
}
