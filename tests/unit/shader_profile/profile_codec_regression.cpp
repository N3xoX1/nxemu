// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_codec.h"
#include <iostream>
#include <stdexcept>
using namespace VideoCommon::ShaderProfile;
void require(bool value) {if(!value)throw std::runtime_error("profile codec regression");}
void rejected(const std::string& value) {
    bool failed=false;try{Unpack(value);}catch(const std::exception&){failed=true;}require(failed);
}
int main() try {
    const std::string json="{\"metadata\":\""+std::string(4096,'a')+"\"}";
    const auto packed=Pack(json);require(Unpack(packed)==json && Unpack(json)==json && packed.size()<json.size());
    for(size_t length:{size_t{8},size_t{31},packed.size()-1})rejected(packed.substr(0,length));
    for(size_t offset:{size_t{8},size_t{12},size_t{23},size_t{24},packed.size()-1}) {
        auto bad=packed;bad[offset]^=1;rejected(bad);
    }
    auto extra=packed+packed.substr(32);
    const auto length=extra.size()-32;
    for(size_t i=0;i<8;++i)extra[24+i]=static_cast<char>(length>>(8*i));
    rejected(extra);
    require(Pack(json)==packed);
    std::cout<<"profile compression roundtrip, deterministic encoding, bounds, checksum and single-frame checks passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
