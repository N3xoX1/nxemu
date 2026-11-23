// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_codec.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <zstd.h>
#include "yuzu_video_core/shader_profile_limits.h"
#include "yuzu_video_core/shader_profile_resources.h"

namespace VideoCommon::ShaderProfile {
namespace {
constexpr size_t Limit=MaxProfilePayloadBytes;
void Check(bool ok) { if(!ok) throw std::runtime_error("Invalid compressed nxshader envelope"); }
void Zcheck(size_t result) { Check(!ZSTD_isError(result)); }
void Put(std::string& out,uint64_t n,size_t size) {
    for(size_t i=0;i<size;++i) out.push_back(static_cast<char>(n>>(8*i)));
}
uint64_t Get(std::string_view bytes,size_t offset,size_t size) {
    Check(offset<=bytes.size() && size<=bytes.size()-offset);uint64_t n{};
    for(size_t i=0;i<size;++i) n|=uint64_t{static_cast<unsigned char>(bytes[offset+i])}<<(8*i);
    return n;
}
// A single standard Zstd frame, known bounded output size, checksum enabled, no external dictionary.
std::string Decompress(std::string_view frame,uint64_t raw_size) {
    Check(raw_size && raw_size<=Limit && frame.size()>=9);
    Check(Get(frame,0,4)==0xfd2fb528U && (static_cast<unsigned char>(frame[4])&4U)!=0);
    const auto frame_size=ZSTD_findFrameCompressedSize(frame.data(),frame.size());Zcheck(frame_size);
    Check(frame_size==frame.size() && ZSTD_getFrameContentSize(frame.data(),frame.size())==raw_size &&
          ZSTD_getDictID_fromFrame(frame.data(),frame.size())==0);
    std::string out(static_cast<size_t>(raw_size),'\0');
    const auto decoded=ZSTD_decompress(out.data(),out.size(),frame.data(),frame.size());Zcheck(decoded);
    Check(decoded==raw_size);return out;
}
} // namespace
std::string Pack(std::string_view payload,int level,uint32_t platform) {
    Check(!payload.empty() && payload.size()<=Limit && level>=1 && level<=19 && (platform==1 || platform==2));
    std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(),ZSTD_freeCCtx);
    Check(context!=nullptr);
    Zcheck(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_compressionLevel,level));
    Zcheck(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_checksumFlag,1));
    std::string compressed(ZSTD_compressBound(payload.size()),'\0');
    const auto size=ZSTD_compress2(context.get(),compressed.data(),compressed.size(),payload.data(),payload.size());
    Zcheck(size);compressed.resize(size);
    std::string out(ProfileMagic);
    Put(out,ProfileMajor,4);Put(out,ProfileMinor,4);Put(out,payload.size(),8);Put(out,size,8);
    Put(out,platform,4);Put(out,ProfileSchema,4);Put(out,0,4);Put(out,0,4);
    out+=compressed;return out;
}
std::string Unpack(std::string_view bytes,ContainerInfo* info) {
    ContainerInfo header;
    std::string payload;
    if(bytes.substr(0,8)==ProfileMagic) {
        Check(bytes.size()>=ProfileHeaderSize);
        header.major=static_cast<uint32_t>(Get(bytes,8,4));header.minor=static_cast<uint32_t>(Get(bytes,12,4));
        if(header.major!=ProfileMajor) throw std::runtime_error("Shader file made by a newer version");
        const auto raw_size=Get(bytes,16,8),stored=Get(bytes,24,8);
        header.platform=static_cast<uint32_t>(Get(bytes,32,4));header.schema=static_cast<uint32_t>(Get(bytes,36,4));
        header.features=static_cast<uint32_t>(Get(bytes,40,4));
        const auto extension_bytes=Get(bytes,44,4);
        Check(header.platform==1 || header.platform==2);
        // A file that needs a feature or a schema this version does not know is refused whole.
        if(header.schema!=ProfileSchema || header.features) throw std::runtime_error("Shader file made by a newer version");
        Check(extension_bytes<=MaxProfileExtensionBytes && extension_bytes<=bytes.size()-ProfileHeaderSize);
        const auto extensions=bytes.substr(ProfileHeaderSize,static_cast<size_t>(extension_bytes));
        // Extensions: tag (u16, nonzero, ascending), size (u16), bytes. A tag with
        // bit 15 set changes how the file is read and must be known.
        for(size_t pos=0,previous=0;pos<extensions.size();) {
            const auto tag=Get(extensions,pos,2),size=Get(extensions,pos+2,2);
            Check(tag>previous && size<=extensions.size()-pos-4);
            if(tag&0x8000) throw std::runtime_error("Shader file made by a newer version");
            previous=static_cast<size_t>(tag);pos+=4+static_cast<size_t>(size);
        }
        header.extensions=std::string(extensions);
        Check(stored==bytes.size()-ProfileHeaderSize-extension_bytes);
        payload=Decompress(bytes.substr(ProfileHeaderSize+static_cast<size_t>(extension_bytes)),raw_size);
    } else {
        // Raw payloads, for offline diagnostic tools.
        Check(bytes.size()<=Limit);
        payload=std::string(bytes);
        header.major=0;
    }
    if(info) *info=std::move(header);
    else if(header.major!=0 && header.platform!=1) throw std::runtime_error("Switch 2 shader file: this emulator runs Switch 1 games");
    return payload;
}
std::string PackFrame(std::string_view raw, int level) {
    // Small frames gain nothing. Contexts are reused per thread: frames are
    // written one at a time and read by the tens of thousands at startup.
    if(raw.size()<64) return {};
    thread_local std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(),ZSTD_freeCCtx);
    if(!context) return {};
    std::string packed(ZSTD_compressBound(raw.size()),'\0');
    const auto size=ZSTD_compressCCtx(context.get(),packed.data(),packed.size(),raw.data(),raw.size(),level);
    if(ZSTD_isError(size) || size>=raw.size()) return {};
    packed.resize(size);
    return packed;
}
bool UnpackFrame(std::string_view bytes, size_t max_size, std::string& out) {
    thread_local std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(),ZSTD_freeDCtx);
    if(!context || ZSTD_findFrameCompressedSize(bytes.data(),bytes.size())!=bytes.size()) return false;
    const auto size=ZSTD_getFrameContentSize(bytes.data(),bytes.size());
    if(size==ZSTD_CONTENTSIZE_UNKNOWN || size==ZSTD_CONTENTSIZE_ERROR || size>max_size) return false;
    out.resize(static_cast<size_t>(size));
    const auto decoded=ZSTD_decompressDCtx(context.get(),out.data(),out.size(),bytes.data(),bytes.size());
    return !ZSTD_isError(decoded) && decoded==size;
}
bool IsZstd(std::string_view bytes) {
    return bytes.size()>=4 && Get(bytes,0,4)==0xfd2fb528U;
}
struct ZstdDictionary::State {
    std::unique_ptr<ZSTD_DDict,decltype(&ZSTD_freeDDict)> dictionary{nullptr,ZSTD_freeDDict};
};
ZstdDictionary::ZstdDictionary(std::string_view bytes) : state(std::make_unique<State>()) {
    id=ZSTD_getDictID_fromDict(bytes.data(),bytes.size());
    if(!id) throw std::runtime_error("Not a zstd dictionary");
    state->dictionary.reset(ZSTD_createDDict(bytes.data(),bytes.size()));
    if(!state->dictionary) throw std::runtime_error("Invalid zstd dictionary");
}
ZstdDictionary::~ZstdDictionary() = default;
unsigned ZstdFrameDictionary(std::string_view frame) {
    ZstdFrameSize(frame,MaxDecodedResource);
    return ZSTD_getDictID_fromFrame(frame.data(),frame.size());
}
size_t ZstdFrameSize(std::string_view frame, size_t max_size) {
    if(!IsZstd(frame) || ZSTD_findFrameCompressedSize(frame.data(),frame.size())!=frame.size())
        throw std::runtime_error("Not a single zstd frame");
    const auto size=ZSTD_getFrameContentSize(frame.data(),frame.size());
    if(size==ZSTD_CONTENTSIZE_UNKNOWN || size==ZSTD_CONTENTSIZE_ERROR || size>max_size)
        throw std::runtime_error("Unsupported zstd frame size");
    return static_cast<size_t>(size);
}
std::string DecodeZstd(std::string_view frame, size_t size, const ZstdDictionary* dictionary) {
    if(ZstdFrameSize(frame,MaxDecodedResource)!=size) throw std::runtime_error("zstd frame size changed");
    const auto id=ZSTD_getDictID_fromFrame(frame.data(),frame.size());
    if(id!=(dictionary?dictionary->Id():0U)) throw std::runtime_error("zstd frame dictionary mismatch");
    thread_local std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(),ZSTD_freeDCtx);
    if(!context) throw std::runtime_error("Cannot create zstd context");
    std::string out(size,'\0');
    const auto decoded=dictionary ?
        ZSTD_decompress_usingDDict(context.get(),out.data(),out.size(),frame.data(),frame.size(),dictionary->state->dictionary.get()) :
        ZSTD_decompressDCtx(context.get(),out.data(),out.size(),frame.data(),frame.size());
    if(ZSTD_isError(decoded) || decoded!=size) throw std::runtime_error("zstd frame failed to decode");
    return out;
}
} // namespace VideoCommon::ShaderProfile
