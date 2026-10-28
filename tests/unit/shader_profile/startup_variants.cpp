// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/startup_pipeline_variants.h"
#include "yuzu_video_core/shader_profile_identity.h"
#include "yuzu_video_core/shader_profile_audit.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

struct CopyKey {
  inline static size_t copies{},moves{};
  int value;
  explicit CopyKey(int v):value(v){}
  CopyKey(const CopyKey& other):value(other.value){++copies;}
  CopyKey(CopyKey&& other) noexcept:value(other.value){++moves;}
  bool operator==(const CopyKey&) const=default;
};
namespace std {
template<>struct hash<CopyKey> {
  size_t operator()(const CopyKey& key) const noexcept {return std::hash<int>{}(key.value);}
};
}
void Check(bool ok) {
  if (!ok)
    throw std::runtime_error("Startup variant safety regression");
}
int main() try {
  // Model texture answers, constant values and HLE replacements as separate
  // compiler inputs. Every conflicting order must quarantine the entire key.
  for (const auto &changed :
       {"texture-type", "constant-value", "hle-replacement"}) {
    for (bool reverse : {false, true}) {
      Vulkan::StartupPipelineVariants<int, std::string> variants;
      const auto equal = [](const auto &a, const auto &b) { return a == b; };
      variants.Observe(1, reverse ? changed : "original", equal);
      variants.Observe(1, reverse ? "original" : changed, equal);
      variants.Observe(1, "original", equal);
      variants.Observe(2, "safe", equal);
      variants.Observe(2, "safe", equal);
      size_t built{};
      const auto rejected =
          variants.ConsumeSafe([&](int key, std::string input) {
            Check(key == 2 && input == "safe");
            ++built;
          });
      Check(built == 1 && rejected == 1);
    }
  }
  // Stable first-use ordering with exact-key deduplication and clean reset.
  {
    Vulkan::StartupPipelineVariants<int, std::string> variants;
    const auto equal = [](const auto &a, const auto &b) { return a == b; };
    for (int key : {17, 3, 91, 17, 3, 42})
      variants.Observe(key, std::to_string(key), equal);
    const std::vector<int> expected{17, 3, 91, 42};
    std::vector<int> actual;
    const auto rejected = variants.ConsumeSafe([&](int key, std::string input) {
      Check(input == std::to_string(key));
      actual.push_back(key);
    });
    Check(rejected == 0 && actual == expected);
    variants.Observe(8, "8", equal);
    variants.Observe(5, "5", equal);
    actual.clear();
    variants.ConsumeSafe([&](int key, std::string) { actual.push_back(key); });
    Check((actual == std::vector<int>{8, 5}));
  }
  {
    Vulkan::StartupPipelineVariants<CopyKey,int> variants;
    const auto equal=[](int a,int b){return a==b;};
    for(int i=0;i<4096;++i)variants.Observe(CopyKey(i),i,equal);
    const auto copies=CopyKey::copies,moves=CopyKey::moves;
    int position{};
    variants.ConsumeSafe([&](const CopyKey& key,int input) {
      Check(key.value==position && input==position);++position;
    });
    Check(position==4096 && copies==CopyKey::copies && moves==CopyKey::moves);
  }
  static_assert(!std::is_copy_constructible_v<Vulkan::StartupPipelineVariants<int,std::string>>);
  static_assert(!std::is_move_constructible_v<Vulkan::StartupPipelineVariants<int,std::string>>);
  {
    Vulkan::StartupPipelineVariants<int,int> variants;
    const auto equal=[](int a,int b){return a==b;};
    std::vector<int> expected;
    // Thousands of insertions force table and order-vector reallocations.
    for(int i=0;i<20000;++i) {
      const int key=(i*7919)%20000;
      variants.Observe(key,key,equal);expected.push_back(key);
    }
    // A late ambiguity remains rejected at its original position.
    variants.Observe(expected[9999],-1,equal);
    size_t cursor{},rejected{};
    Check(variants.ConsumeSafe([&](int key,int input) {
      if(cursor==9999)++cursor;
      Check(key==expected[cursor++] && input==key);
    },[&](int key){Check(key==expected[9999]);++rejected;})==1);
    Check(cursor==expected.size() && rejected==1);
    // Callbacks may add next-batch work without invalidating this batch.
    variants.Observe(7,7,equal);variants.Observe(2,2,equal);
    std::vector<int> first,second;
    variants.ConsumeSafe([&](int key,int) {
      first.push_back(key);variants.Observe(key+100,key,equal);
    });
    variants.ConsumeSafe([&](int key,int){second.push_back(key);});
    Check(first==std::vector<int>({7,2}) && second==std::vector<int>({107,102}));
    variants.Observe(9,9,equal);variants.Observe(4,4,equal);
    bool threw=false;
    try {variants.ConsumeSafe([](int,int){throw std::runtime_error("callback");});}
    catch(const std::runtime_error&){threw=true;}
    Check(threw);
    variants.ConsumeSafe([](int,int){Check(false);}); // No retry with moved inputs.
    variants.Observe(3,3,equal);
    variants.ConsumeSafe([](int key,int input){Check(key==3 && input==3);});
    variants.Observe(8,8,equal);variants.Observe(8,9,equal);
    threw=false;
    try {variants.ConsumeSafe([](int,int){Check(false);},[](int){throw std::runtime_error("reject");});}
    catch(const std::runtime_error&){threw=true;}
    Check(threw);
    variants.Observe(5,5,equal);
    variants.ConsumeSafe([](int key,int){Check(key==5);});
  }
  // Equal generated code alone cannot prove equivalent binding metadata.
  VideoCommon::ShaderProfile::Audit::Capture first, second;
  first.spirv = second.spirv = {"same-code-hash"};
  first.interfaces.emplace_back(0, Shader::Info{});
  second.interfaces = first.interfaces;
  Check(first == second);
  second.interfaces[0].second.constant_buffer_used_sizes[0] = 16;
  Check(first != second);
  second = first;
  second.interfaces[0].second.loads.mask.set(1);
  Check(first != second);
  second = first;
  second.rasterizes_lines = true;
  Check(first != second);
  using VideoCommon::ShaderProfile::ImportIdentity;
  const auto stamp = ImportIdentity("profile", "base+update-v1", 1, 63, 20);
  Check(!stamp.empty());
  Check(stamp != ImportIdentity("profile", "base+update-v2", 1, 63, 20));
  Check(stamp != ImportIdentity("profile", "base+update-v1", 2, 63, 20));
  Check(stamp != ImportIdentity("profile", "base+update-v1", 1, 31, 20));
  Check(stamp != ImportIdentity("profile", "base+update-v1", 1, 63, 21));
  Check(stamp != ImportIdentity("other-profile", "base+update-v1", 1, 63, 20));
  Check(ImportIdentity("profile", "", 1, 63, 20).empty());
  std::cout << "PASS: ambiguous inputs never win by order; import identity "
               "isolates content, title, GPU, ABI and profile\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
