#!/usr/bin/env python3
"""Production write preparation/retirement vs an independent 4 KiB ownership oracle."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4];source=(root/'src/nxemu-os/core/memory.cpp').read_text()
def method(name):
 a=source.index('    '+name);b=source.index('{',a);e=b+1;depth=1
 while depth:
  depth+=(source[e]=='{')-(source[e]=='}');e+=1
 return source[a:e]
code=r'''
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>
using u64=uint64_t;using u8=uint8_t;constexpr u64 YUZU_PAGESIZE=4096,YUZU_PAGEBITS=12,YUZU_PAGEMASK=4095,NceHostPageSize=16384;
void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
namespace Common{
 enum class PageType{Memory,RasterizerCachedMemory};enum class MemoryPermission{Read=1,ReadWrite=3};
 template<class T>auto make_unique_for_overwrite(){return std::make_unique_for_overwrite<T>();}
}
namespace NXVideoSetting{constexpr int EnableReactiveFlushing=0;}
struct Settings{bool reactive=true;bool GetBool(int)const{return reactive;}}settings,*g_settings=&settings;
struct Buffer{
 std::array<bool,4>gr{true,true,true,true},gw{true,true,true,true},nr{true,true,true,true},nw{true,true,true,true};bool contiguous=false;
 unsigned protections=0;bool revoke_after_copy=false;
 bool NceAccessAllowed(u64 a,bool w)const{return a<16384&&(w?gw:gr)[a/4096];}
 bool NceTrackedAccessAllowed(u64 a,bool w){if(!w&&revoke_after_copy){nr[a/4096]=false;revoke_after_copy=false;}return NceAccessAllowed(a,w)&&(w?nw:nr)[a/4096];}
 bool NceHostAccessAllowed(u64,bool w)const{if(!contiguous)return false;for(unsigned i=0;i<4;++i)if(!(w?gw[i]&&nw[i]:gr[i]&&nr[i]))return false;return true;}
 void ProtectAccess(u64 a,u64 n,Common::MemoryPermission permission){++protections;unsigned p=unsigned(permission);for(u64 x=a;x<a+n;x+=4096){nr[x/4096]=p&1;nw[x/4096]=p&2;}}
};
struct PageInfo{Common::PageType type=Common::PageType::Memory;auto Type()const{return type;}};
struct Table{std::array<PageInfo,4>pointers;};
struct Impl{
 using PageCopy=std::array<u8,4096>;Buffer storage;Buffer*buffer=&storage;Table table;Table*current_page_table=&table;
 std::mutex nce_write_mutex;std::set<u64>nce_open_read_pages;std::map<u64,unsigned>nce_read_cached;std::map<u64,std::unique_ptr<PageCopy>>nce_written;
 std::array<PageCopy,4>ram{},gpu{};u64 downloads=0,bytes=0,uploads=0;bool revoke_on_download=false;
 bool PreciseNceReadTracking()const{return false;}bool DemandNceReadback()const{return false;}
 void RearmNceReads(){}
 u8*GetPointerFromRasterizerCachedMemory(u64 a){return a<16384?ram[a/4096].data()+a%4096:nullptr;}
 void HandleRasterizerDownload(u64 a,u64 n){++downloads;bytes+=n;for(u64 x=a;x<a+n;++x){auto old=nce_written.find((x/4096)*4096);if(old==nce_written.end()||ram[x/4096][x%4096]==(*old->second)[x%4096])ram[x/4096][x%4096]=gpu[x/4096][x%4096];}if(revoke_on_download)storage.revoke_after_copy=true;}
 void HandleRasterizerWrite(u64 a,u64 n){++uploads;for(u64 x=a;x<a+n;++x)gpu[x/4096][x%4096]=ram[x/4096][x%4096];}
 void ProtectNceReadCached(u64 a,u64 n){for(u64 p=a;p<a+n;p+=4096)buffer->ProtectAccess(p,4096,nce_read_cached.contains(p)?Common::MemoryPermission{}:Common::MemoryPermission::Read);}
 void Setup(unsigned cache){for(unsigned p=0;p<4;++p){for(unsigned i=0;i<4096;++i){gpu[p][i]=(p*73+i*17)&255;ram[p][i]=gpu[p][i];}if(cache&(1u<<p)){table.pointers[p].type=Common::PageType::RasterizerCachedMemory;nce_read_cached[p*4096]=1;storage.nr[p]=storage.nw[p]=false;}}}
'''+method('void DownloadNceHostPage(')+method('bool NceTrackWrites(')+method('void FlushNceWrites(')+r'''
};
int main(){
 unsigned cases=0;
 for(unsigned cache=0;cache<16;++cache)for(unsigned target=0;target<4;++target)for(unsigned permissions=0;permissions<4;++permissions)for(bool contiguous:{false,true}){
  Impl m;m.Setup(cache);m.storage.contiguous=contiguous;m.storage.gr[target]=permissions&1;m.storage.gw[target]=permissions&2;
  auto gpu_before=m.gpu;bool ready=m.NceTrackWrites(target*4096+128,true);
  require(ready==bool(permissions==3),"canonical lease follows target guest permissions, independent of neighbour/contiguity");
  require(m.gpu==gpu_before,"preparing an access does not write GPU data");
  for(unsigned p=0;p<4;++p)if(p!=target){require(!m.nce_written.contains(p*4096),"canonical lease snapshots no neighbour");require(!m.storage.nw[p]||!(cache&(1u<<p)),"canonical lease never grants cached neighbours write access");}
  if(ready){require(m.ram[target]==m.gpu[target],"lease starts with current target bytes");if(cache&(1u<<target))require(m.nce_written.contains(target*4096),"cached write has snapshot before permission grant");auto downloads=m.downloads;require(m.NceTrackWrites(target*4096+128,true),"ready lease reusable");require(m.downloads==downloads,"repeated ready writes perform no new readback");}
  ++cases;
 }
 // Changed bytes survive later GPU publication; other bytes retain the GPU update.
 Impl m;m.Setup(15);require(m.NceTrackWrites(0,true),"first cached lease");m.ram[0][7]=0x91;
 m.gpu[0][300]^=0x5a;m.HandleRasterizerDownload(0,4096);
 require(m.ram[0][7]==0x91&&m.ram[0][300]==m.gpu[0][300],"CPU dirty bytes merge with newer GPU clean bytes");
 m.FlushNceWrites();require(m.gpu[0][7]==0x91,"retirement publishes CPU write");require(!m.storage.nw[0]&&m.nce_written.empty(),"retirement revokes lease before snapshot release");
 m.gpu[0][20]=0x27;require(m.NceTrackWrites(0,true)&&m.ram[0][20]==0x27,"new GPU generation reacquired before partial store");
 m.ram[0][9]=0x84;m.FlushNceWrites();require(m.gpu[0][9]==0x84&&m.gpu[0][20]==0x27,"new GPU bytes and CPU partial store coexist");
 // A late revocation is observed rather than returning permission to use stale data.
 Impl late;late.Setup(15);late.revoke_on_download=true;require(!late.NceTrackWrites(0,true),"late GPU revoke prevents native store lease");
 late.revoke_on_download=false;require(late.NceTrackWrites(0,true),"revoked lease can synchronize again");
 // Guest-denied writes never allocate snapshots or open another guest page.
 Impl denied;denied.Setup(15);denied.storage.gw[0]=false;require(!denied.NceTrackWrites(0,true)&&denied.downloads==0&&denied.nce_written.empty(),"write denial performs no synchronization or allocation");
 // Equivalent warm stores to uncached data sharing a fragmented host block with
 // three GPU textures. Counts are production operations, not predicted game FPS.
 constexpr unsigned stores=50000;u64 old_downloads=0,new_downloads=0,old_bytes=0,new_bytes=0;double elapsed[2]{};
 for(unsigned canonical=0;canonical<2;++canonical){Impl p;p.Setup(14);auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<stores;++i){p.NceTrackWrites(128,canonical);p.ram[0][128]=u8(i);}elapsed[canonical]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();if(canonical){new_downloads=p.downloads;new_bytes=p.bytes;}else{old_downloads=p.downloads;old_bytes=p.bytes;}require(p.ram[0][128]==u8(stores-1),"identical stores completed");}
 require(old_downloads==stores*3&&new_downloads==0,"shared host-page neighbour traffic removed entirely for owned target");
 printf("PASS %u permission/cache/layout cases; snapshot lifecycle, CPU/GPU merge, late revoke and guest denial\n",cases);
 printf("WORKLOAD %u same-page stores: legacy downloads=%llu bytes=%llu ms=%.3f; canonical downloads=%llu bytes=%llu ms=%.3f (CPU mock, no GPU/FPS prediction)\n",stores,(unsigned long long)old_downloads,(unsigned long long)old_bytes,elapsed[0],(unsigned long long)new_downloads,(unsigned long long)new_bytes,elapsed[1]);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-canonical-write-')as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
