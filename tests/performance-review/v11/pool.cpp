#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>
using u64=uint64_t;
struct Semaphore {
 std::atomic<u64> known{};u64 actual{},current=1,refreshes{};
 VkDevice device{};VkSemaphore semaphore{};
 u64 KnownGpuTick()const{return known.load(std::memory_order_acquire);}
 u64 CurrentTick()const{return current;}
 void Refresh(){++refreshes;u64 tick=actual;if(device && vkGetSemaphoreCounterValue(device,semaphore,&tick)!=VK_SUCCESS)throw std::runtime_error("counter failed");known.store(tick,std::memory_order_release);}
};
struct Pool {
 Semaphore* master_semaphore{};std::vector<u64> ticks;size_t hint_iterator{},grow_step=16;
 size_t Baseline();size_t CommitResource();
 size_t ManageOverflow(){const size_t before=ticks.size();ticks.resize(before+grow_step);return before;}
};
#include "pool-base.inc"
#define STRING2(x) #x
#define STRING(x) STRING2(x)
#include STRING(POOL_INC)
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
volatile u64 sink;
int main(int argc,char** argv)try {
 std::mt19937 r(71221);u64 cases=0;
 for(unsigned trial=0;trial<200;++trial){Semaphore a,b;Pool p{&a},q{&b};p.grow_step=q.grow_step=1+r()%48;for(unsigned i=0;i<12000;++i){if(r()%3==0)++a.current;a.actual=std::min(a.current-1,a.actual+r()%3);b.current=a.current;b.actual=a.actual;if(r()%5==0){a.known.store(a.actual);b.known.store(b.actual);}const auto x=p.Baseline(),y=q.CommitResource();require(x==y,"resource selection differs");require(p.ticks==q.ticks && p.hint_iterator==q.hint_iterator,"pool state differs");require(q.ticks[y]==b.current,"reused resource not protected");++cases;}}
 std::cout<<"PASS "<<cases<<" exact selection/tick/wrap/growth cases; partial GPU completion and stale known ticks\n";
 VkInstance instance{};VkDevice device{};VkSemaphore semaphore{};
 if(argc>1){VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;require(vkCreateInstance(&ci,nullptr,&instance)==VK_SUCCESS,"instance");uint32_t count{};vkEnumeratePhysicalDevices(instance,&count,nullptr);std::vector<VkPhysicalDevice> gpus(count);vkEnumeratePhysicalDevices(instance,&count,gpus.data());uint32_t n{};vkGetPhysicalDeviceQueueFamilyProperties(gpus[0],&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(gpus[0],&n,families.data());uint32_t family=0;while(!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};timeline.timelineSemaphore=VK_TRUE;VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&timeline;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&queue;require(vkCreateDevice(gpus[0],&dci,nullptr,&device)==VK_SUCCESS,"device");VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;type.initialValue=10000000;VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sci.pNext=&type;require(vkCreateSemaphore(device,&sci,nullptr,&semaphore)==VK_SUCCESS,"semaphore");}
 for(unsigned capacity:{16U,256U,4096U})for(unsigned load:{0U,1U,2U}){std::vector<double> measurements[2];u64 reads[2]{};
  for(unsigned round=0;round<11;++round)for(unsigned order=0;order<2;++order){const unsigned mode=(round+order)%2;Semaphore s;s.device=device;s.semaphore=semaphore;s.current=10000001;s.actual=10000000;s.known.store(load==0?s.actual:0);Pool p{&s};p.ticks.resize(capacity);const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<100000;++i){if(load==0)p.ticks[p.hint_iterator]=1;if(load==1 && i%32==0)for(auto& t:p.ticks)t=1;if(load==2 && i%128==0)for(size_t j=0;j<p.ticks.size();j+=3)p.ticks[j]=1;sink=mode?p.CommitResource():p.Baseline();}measurements[mode].push_back(std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000);reads[mode]=s.refreshes;}
  std::cout<<"BENCH capacity="<<capacity<<" load="<<load;for(auto& m:measurements){std::sort(m.begin(),m.end());std::cout<<' '<<m[5];}std::cout<<" refreshes="<<reads[0]<<','<<reads[1]<<'\n';
 }
 if(device){vkDestroySemaphore(device,semaphore,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);}return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
