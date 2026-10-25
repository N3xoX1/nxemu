#!/usr/bin/env python3
"""Run production NceTrackReads against independent guest/host permission states."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[4]
s=(ROOT/'src/nxemu-os/core/memory.cpp').read_text();a=s.index('bool Memory::NceTrackReads(uint64_t vaddr)');b=s.index('\nbool Memory::NceTrackWrites(',a)
code=r'''
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using u64=uint64_t;
void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(99);}}
struct Buffer{
 std::array<bool,4>ready{};bool contiguous=true;
 bool NceTrackedAccessAllowed(u64 a,bool)const{return ready[a/4096];}
 bool NceHostAccessAllowed(u64,bool)const{return contiguous&&ready[0]&&ready[1]&&ready[2]&&ready[3];}
};
struct Impl{
 Buffer storage;Buffer*buffer=&storage;unsigned requests=0,copies=0;bool late_gpu_write=false;
 std::array<unsigned,4>gpu{10,20,30,40},ram{10,20,30,40};
 void DownloadNceHostPage(u64 a){
  ++requests;for(unsigned p=0;p<4;++p)if(!storage.ready[p]){ram[p]=gpu[p];storage.ready[p]=true;++copies;}
  if(late_gpu_write){storage.ready[a/4096]=false;++gpu[a/4096];}
 }
};
struct Memory{Impl*impl;bool NceTrackReads(uint64_t);};
'''+s[a:b]+r'''
int main(){
 const bool fast=std::getenv("NXEMU_NCE_PAGE_READ_FASTPATH")!=nullptr;unsigned cases=0,skipped=0;
 for(unsigned mask=0;mask<16;++mask)for(unsigned p=0;p<4;++p)for(bool direct:{false,true}){
  Impl im;Memory m{&im};im.storage.contiguous=direct;
  for(unsigned j=0;j<4;++j){im.storage.ready[j]=(mask>>j)&1;if(!im.storage.ready[j])im.ram[j]=0;}
  bool skip=fast&&bool(mask&(1U<<p));bool resumes=m.NceTrackReads(p*4096);
  require(im.requests==(skip?0:1),"synchronize only if target needs GPU data");
  require(resumes==(direct&&(skip?mask==15:true)),"native resume uses HOST permission, never guest readiness alone");
  require(im.ram[p]==im.gpu[p],"the emulated or native load sees fresh target data");
  if(skip){require(im.copies==0,"false neighbor fault copies nothing");for(unsigned j=0;j<4;++j)require(im.storage.ready[j]==bool(mask&(1U<<j)),"unrelated GPU ownership preserved");++skipped;}
  ++cases;
 }
 Impl race;Memory m{&race};race.late_gpu_write=true;
 require(!m.NceTrackReads(0),"GPU write after copy prevents native resume");
 race.late_gpu_write=false;require(m.NceTrackReads(0)&&race.ram[0]==race.gpu[0],"next true dirty read sees new GPU generation");
 race.storage.ready[0]=false;race.gpu[0]=99;
 require(m.NceTrackReads(0)&&race.ram[0]==99,"new write revokes a previously current page");
 printf("PASS %u mixed host/guest permission combinations, %u avoided false-fault requests; late GPU write and generation checks; fast=%d\n",cases,skipped,fast);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-page-faults-')as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 for mode in (False,True):
  env=dict(os.environ);env.pop('NXEMU_NCE_PAGE_READ_FASTPATH',None);env.pop('NXEMU_NCE_READBACK_PROFILE',None)
  if mode:env['NXEMU_NCE_PAGE_READ_FASTPATH']='1'
  subprocess.run([str(p/'test')],env=env,check=True)
