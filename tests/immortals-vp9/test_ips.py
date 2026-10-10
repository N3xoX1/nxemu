"""Exercise NXEmu's production IPS parser on the actual generated record."""
import pathlib, sys
root=pathlib.Path(__file__).resolve().parents[2]
source=(root/'src/nxemu-loader/core/file_sys/ips_layer.cpp').read_text()
body=source[source.index('enum class IPSFileType'):source.index('struct IPSwitchCompiler::IPSwitchPatch')]
prefix=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;
namespace Common { u16 swap16(u16 x){return u16((x>>8)|(x<<8));} }
namespace FileSys {
struct VectorVfsFile;
using VirtualFile=std::shared_ptr<VectorVfsFile>;
struct VectorVfsFile {
 std::vector<u8> bytes;
 explicit VectorVfsFile(std::vector<u8> data,std::string={},void* =nullptr):bytes(std::move(data)){}
 size_t Read(void* p,size_t size,uint64_t off) const {
  if(off>=bytes.size())return 0;
  size=std::min(size,bytes.size()-size_t(off));std::memcpy(p,bytes.data()+off,size);return size;
 }
 std::vector<u8> ReadBytes(size_t size,uint64_t off=0) const {
  std::vector<u8> data(size);data.resize(Read(data.data(),size,off));return data;
 }
 template<class T>size_t ReadObject(T* p,uint64_t off)const{return Read(p,sizeof(T),off);}
 std::optional<u8> ReadByte(uint64_t off)const {if(off>=bytes.size())return {};return bytes[off];}
 std::vector<u8> ReadAllBytes()const{return bytes;}
 std::string GetName()const{return "main";}
 void* GetContainingDirectory()const{return nullptr;}
};
'''
suffix=r'''
}
int main(int argc,char** argv){
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);
 std::vector<u8> ips((std::istreambuf_iterator<char>(f)),{});assert(ips.size()==49);
 constexpr size_t offset=0x4b4340;
 std::vector<u8> image(offset+36+128,0xa5);
 auto input=std::make_shared<FileSys::VectorVfsFile>(image);
 auto patch=std::make_shared<FileSys::VectorVfsFile>(ips);
 auto result=FileSys::PatchIPS(input,patch);assert(result);
 for(size_t i=0;i<image.size();++i){
  u8 expected=(i>=offset && i<offset+36)?ips[10+i-offset]:0xa5;
  assert(result->bytes[i]==expected);
 }
 assert(input->bytes==image);
 patch->bytes.pop_back();assert(!FileSys::PatchIPS(input,patch));
 std::puts("PASS production IPS parser: exact 36-byte record, adjacent bytes unchanged, original unchanged, truncated record rejected");
}
'''
out=root/'build/pr396-corrections/immortal-freeze/ips-production-test.cpp'
out.write_text(prefix+body+suffix);print(out)
