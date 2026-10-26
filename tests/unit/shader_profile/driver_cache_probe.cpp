// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/driver_pipeline_cache_probe.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

static void Check(bool ok) { if(!ok)throw std::runtime_error("driver cache probe regression"); }
int main() try {
    using namespace Vulkan::DriverCacheProbe;
    for(const char* option:{static_cast<const char*>(nullptr),"","0","true","01","1 "})
        Check(!EnabledFromOption(option));
    Check(EnabledFromOption("1"));
    VkPhysicalDeviceProperties device{};device.vendorID=17;device.deviceID=31;
    for(size_t i=0;i<VK_UUID_SIZE;++i)device.pipelineCacheUUID[i]=static_cast<uint8_t>(i+1);
    VkPipelineCacheHeaderVersionOne header{sizeof(header),VK_PIPELINE_CACHE_HEADER_VERSION_ONE,17,31,{}};
    std::memcpy(header.pipelineCacheUUID,device.pipelineCacheUUID,VK_UUID_SIZE);
    std::array<char,sizeof(header)+17> storage{};
    auto expect=[&](std::string_view result,size_t size=sizeof(VkPipelineCacheHeaderVersionOne)){
        std::memcpy(storage.data()+1,&header,sizeof(header));
        Check(Header(std::span<const char>(storage.data()+1,size),device)==result);
        VkPipelineCacheHeaderVersionOne unchanged{};
        std::memcpy(&unchanged,storage.data()+1,sizeof(header));
        Check(std::memcmp(&unchanged,&header,sizeof(header))==0);
    };
    for(size_t n=0;n<sizeof(header);++n)expect("short",n);
    expect("compatible");expect("compatible",sizeof(header)+16);
    ++header.vendorID;expect("different-device");--header.vendorID;
    ++header.deviceID;expect("different-device");--header.deviceID;
    header.pipelineCacheUUID[5]^=1;expect("different-uuid");header.pipelineCacheUUID[5]^=1;
    header.headerVersion=static_cast<VkPipelineCacheHeaderVersion>(77);expect("invalid");
    header.headerVersion=VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    header.headerSize=sizeof(header)-1;expect("invalid");
    header.headerSize=sizeof(header)+1;expect("invalid");expect("compatible",sizeof(header)+1);
    header.headerSize=0xffffffff;expect("invalid");
    std::cout<<"PASS: opt-in, truncated/unaligned headers, size, version, vendor/device and UUID classification\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
