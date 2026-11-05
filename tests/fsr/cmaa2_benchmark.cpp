// SPDX-License-Identifier: GPL-2.0-or-later
#include "cmaa2_fixture.h"

int main(int argc,char** argv) try {
    if(argc!=2 && argc!=4) throw std::runtime_error("Usage: nxemu-cmaa2-vulkan-benchmark OUTPUT.csv [WIDTH HEIGHT]");
    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,true);
    std::cout<<vk.properties.deviceName<<'\n';
    std::ofstream report(argv[1]);report<<"mode,width,height,pattern,gpu_median_ms,gpu_p95_ms,changed_rgb_channels\n";
    { CMAA aa(vk);
      std::vector<std::pair<int,int>> dimensions{{1,1},{3,5},{1279,719},{1920,1080},{3840,2160}};
      if(argc==4) {
          const int w=std::stoi(argv[2]),h=std::stoi(argv[3]);
          if(w<1 || h<1 || w>4096 || h>4096) throw std::runtime_error("Benchmark size must be 1..4096");
          dimensions={{w,h}};
      }
      for(auto size:dimensions)
          for(int kind=0;kind<3;++kind) aa.Run(size.first,size.second,kind,report);
    }
    if(validation_errors) throw std::runtime_error("Vulkan validation errors: "+std::to_string(validation_errors));
    std::cout<<"PASS: finite output, alpha, constant images and frame reuse; validation="
             <<(std::getenv("NXEMU_FSR_VALIDATION")?"enabled":"disabled")<<'\n';
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
