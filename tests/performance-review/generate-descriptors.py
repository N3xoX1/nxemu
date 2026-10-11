"""Extend the existing real-GPU storage-array fixture with descriptor-update timings.

The production-emitted shader still uses five handles. Larger layouts measure
the driver CPU cost of the mostly-null typed TIC arrays, not shader throughput.
"""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[2]
out = root / 'build/performance-review-20261011'
out.mkdir(parents=True, exist_ok=True)
original = (root / 'tests/immortals-gpu/storage-array.cpp').read_text()
source = original

def replace_once(old, new):
    global source
    assert source.count(old) == 1, old
    source = source.replace(old, new)

replace_once('#include "../pr396/vulkan-synthetic.cpp"',
             '#include "' + (root / 'tests/pr396/vulkan-synthetic.cpp').as_posix() + '"\n#include <chrono>')
replace_once(' if(argc!=3)return 2;',
             ' if(argc!=4)return 2; const uint32_t entries=std::stoul(argv[3]); if(entries<5)return 2;')
replace_once('VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,5,VK_SHADER_STAGE_COMPUTE_BIT',
             'VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,entries,VK_SHADER_STAGE_COMPUTE_BIT')
replace_once('VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,15}', 'VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,entries*3}')
replace_once('std::array<VkDescriptorImageInfo,15> infos{};', 'std::vector<VkDescriptorImageInfo> infos(entries*3);')
replace_once('i<5;++i)infos[type*5+i]', 'i<entries;++i)infos[type*entries+i]')
replace_once('write.descriptorCount=5;', 'write.descriptorCount=entries;')
replace_once('write.pImageInfo=&infos[type*5];', 'write.pImageInfo=&infos[type*entries];')
replace_once(' f.Begin();auto cb=f.commands[1];', '''
 static_assert(sizeof(VkDescriptorImageInfo)==sizeof(VkDescriptorBufferInfo));
 std::vector<unsigned char> payload((entries*3+1)*sizeof(VkDescriptorImageInfo));
 std::memcpy(payload.data(),infos.data(),infos.size()*sizeof(VkDescriptorImageInfo));
 std::memcpy(payload.data()+infos.size()*sizeof(VkDescriptorImageInfo),&mask_info,sizeof(mask_info));
 std::array<VkDescriptorUpdateTemplateEntry,4> template_entries{};
 for(uint32_t i=0;i<4;++i){auto& e=template_entries[i];e.dstBinding=i;e.descriptorCount=i<3?entries:1;
  e.descriptorType=i<3?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  e.offset=size_t(i)*entries*sizeof(VkDescriptorImageInfo);e.stride=sizeof(VkDescriptorImageInfo);}
 VkDescriptorUpdateTemplate update_template{};
 VkDescriptorUpdateTemplateCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
 tci.descriptorUpdateEntryCount=4;tci.pDescriptorUpdateEntries=template_entries.data();
 tci.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;tci.descriptorSetLayout=layout;
 Check(vkCreateDescriptorUpdateTemplate(f.device,&tci,nullptr,&update_template));
 for(bool templated:{false,true}){
  std::vector<double> samples;
  for(uint32_t run=0;run<25;++run){
   auto start=std::chrono::steady_clock::now();
   for(uint32_t repeat=0;repeat<64;++repeat){
    if(templated)vkUpdateDescriptorSetWithTemplate(f.device,set,update_template,payload.data());
    else vkUpdateDescriptorSets(f.device,4,writes.data(),0,nullptr);
   }
   auto end=std::chrono::steady_clock::now();
   if(run>=4)samples.push_back(std::chrono::duration<double,std::micro>(end-start).count()/64);
  }
  std::sort(samples.begin(),samples.end());
  std::cout<<"DESCRIPTORS_BENCH method="<<(templated?"template":"writes")<<" typed_entries="<<entries*3
           <<" update_wall_us="<<samples[10]<<" min="<<samples.front()<<" max="<<samples.back()<<" samples=21\\n";
 }
 vkDestroyDescriptorUpdateTemplate(f.device,update_template,nullptr);
 f.Begin();auto cb=f.commands[1];''')
(out / 'descriptors.cpp').write_text(source)
(out / 'descriptors-extraction.json').write_text(json.dumps({
    'original_fixture_sha256': hashlib.sha256(original.encode()).hexdigest(),
    'generated_sha256': hashlib.sha256(source.encode()).hexdigest(),
}, indent=2))
print('PASS generated descriptor-update benchmark from storage-array fixture')
