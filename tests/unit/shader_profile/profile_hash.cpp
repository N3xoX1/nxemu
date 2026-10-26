// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_file_stamp.h"
#include <limits>
#include <random>
#include <stdexcept>
int main() {
    using VideoCommon::ShaderProfile::ResourceSha256;
    using VideoCommon::ShaderProfile::FileTimestamp;
    using Time = std::filesystem::file_time_type;
    using Rep = Time::duration::rep;
    const auto stamp = [](Rep ticks) { return FileTimestamp(Time(Time::duration(ticks))); };
    for (long long ticks : {0LL, 1LL, -1LL, 1792800000000000000LL, -1792800000000000000LL}) {
        if (stamp(ticks) != std::to_string(ticks))
            throw std::runtime_error("File timestamp precision lost");
    }
    if constexpr (sizeof(Rep) > sizeof(long long)) {
        const Rep wide = static_cast<Rep>(std::numeric_limits<long long>::max()) + 1;
        if (stamp(wide) != "9223372036854775808" || stamp(-wide) != "-9223372036854775808")
            throw std::runtime_error("Wide file timestamp narrowed");
    }
    if(ResourceSha256("abc")!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        throw std::runtime_error("SHA-256 standard vector mismatch");
    std::mt19937 random(123);
    for(size_t size:{0U,1U,7U,55U,56U,63U,64U,65U,127U,128U,129U,4096U,1048576U}) {
        std::string data(size,'\0');
        for(auto& value:data) value=static_cast<char>(random());
        const auto raw=VideoCommon::ShaderProfile::ResourceDigest(data);
        const auto hex=ResourceSha256(data);
        constexpr char digits[]="0123456789abcdef";
        for(size_t i=0;i<raw.size();++i)
            if(hex[i*2]!=digits[raw[i]>>4] || hex[i*2+1]!=digits[raw[i]&15])
                throw std::runtime_error("Raw SHA-256 digest mismatch");
        if(hex!=sha256(data)) throw std::runtime_error("platform SHA-256 mismatch");
    }
}
