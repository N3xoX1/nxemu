// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VideoCommon::ShaderProfile {
inline constexpr size_t MaxDecodedResource=512*1024*1024;
inline void ResourceCheck(bool value,const char* reason) {
    if(!value) throw std::runtime_error(reason);
}
inline void CopyMatch(std::span<char> out,size_t& output,size_t distance,size_t length) {
    ResourceCheck(distance && distance<=output && length<=out.size()-output,"Invalid compression backreference");
    const size_t source=output-distance;
    // Copy only initialized bytes, doubling the available repeated pattern each step.
    // Every memcpy is non-overlapping, including distance-one repetitions.
    while(length) {
        const auto count=std::min(length,output-source);
        std::memcpy(out.data()+output,out.data()+source,count);
        output+=count;length-=count;
    }
}
inline void DecodeYaz0Into(std::string_view raw,std::span<char> out,size_t* consumed=nullptr) {
    ResourceCheck(raw.size()>=16 && raw.substr(0,4)=="Yaz0","Invalid Yaz0 resource");
    const auto byte=[&](size_t pos){ ResourceCheck(pos<raw.size(),"Truncated Yaz0"); return static_cast<unsigned char>(raw[pos]); };
    const size_t full_size=(size_t{byte(4)}<<24)|(size_t{byte(5)}<<16)|(size_t{byte(6)}<<8)|byte(7);
    ResourceCheck(full_size<=MaxDecodedResource,"Yaz0 output too large");
    const auto size=out.size();
    ResourceCheck(size<=full_size,"Yaz0 prefix outside resource");
    size_t cursor=16,output{};
    while (output<size) {
        const auto flags=byte(cursor++);
        if(flags==0xff) {
            const auto count=std::min(size_t{8},size-output);
            ResourceCheck(count<=raw.size()-cursor,"Truncated Yaz0 literals");
            std::memcpy(out.data()+output,raw.data()+cursor,count);
            output+=count;cursor+=count;
            continue;
        }
        for (unsigned bit=0;bit<8 && output<size;++bit) {
            if (flags & (128U>>bit)) out[output++]=static_cast<char>(byte(cursor++));
            else {
                const auto a=byte(cursor);
                ++cursor;
                const auto b=byte(cursor);
                ++cursor;
                const size_t distance=((a&15U)<<8)+b+1;
                size_t length=a>>4;
                if (!length) length=byte(cursor++)+18U; else length+=2;
                ResourceCheck(length<=full_size-output,"Invalid Yaz0 backreference");
                CopyMatch(out,output,distance,std::min(length,size-output));
            }
        }
    }
    if(consumed) *consumed=cursor;
}
inline std::string DecodeYaz0(std::string_view raw,size_t prefix=MaxDecodedResource+1) {
    ResourceCheck(raw.size()>=16 && raw.substr(0,4)=="Yaz0","Invalid Yaz0 resource");
    const auto* bytes=reinterpret_cast<const unsigned char*>(raw.data());
    const size_t full_size=(size_t{bytes[4]}<<24)|(size_t{bytes[5]}<<16)|(size_t{bytes[6]}<<8)|bytes[7];
    const auto size=prefix==MaxDecodedResource+1?full_size:prefix;
    ResourceCheck(full_size<=MaxDecodedResource && size<=full_size,"Yaz0 output bounds");
    std::string out(size,'\0'); DecodeYaz0Into(raw,out); return out;
}
inline std::string DecodeLz4(std::string_view raw, size_t size) {
    ResourceCheck(size<=MaxDecodedResource,"LZ4 output too large"); std::string out(size,'\0'); size_t cursor{},output{};
    const auto byte=[&](){ ResourceCheck(cursor<raw.size(),"Truncated LZ4"); return static_cast<unsigned char>(raw[cursor++]); };
    const auto length=[&](size_t n){
        if(n==15) { unsigned more; do { more=byte(); n+=more; ResourceCheck(n<=size,"LZ4 length overflow"); } while(more==255); }
        return n;
    };
    while(cursor<raw.size()) {
        const auto token=byte(); const auto literals=length(token>>4);
        ResourceCheck(literals<=raw.size()-cursor && literals<=size-output,"LZ4 literal bounds");
        std::memcpy(out.data()+output,raw.data()+cursor,literals);cursor+=literals;output+=literals;
        if(cursor==raw.size()) break;
        const auto low=byte(); const size_t distance=low+(size_t{byte()}<<8);
        const auto count=length(token&15U)+4;
        CopyMatch(out,output,distance,count);
    }
    ResourceCheck(output==size,"LZ4 output size mismatch"); return out;
}
} // namespace VideoCommon::ShaderProfile
