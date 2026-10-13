"""Execute captured ARM64 cancellation code with modeled SDK locking."""
import pathlib,sys,struct
root=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'build/pr396-corrections/immortal-freeze/python-deps'))
from dump_memory import guest_read
from patch import patch_bytes
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import *
FUNC=0x80addfa0; STOP=0x70001000; B=0x1185fb64c0; A=0x1185f3c8a0
ARG=struct.unpack('<Q',guest_read(0x1185fb4d20,8))[0]
def branch(src,dst):return struct.pack('<I',0x14000000|(((dst-src)//4)&0x3ffffff))
PATCH=patch_bytes()
class Replay:
 def __init__(self,patched,busy,completed):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.pages=set();self.calls=[];self.blocked=False;self.steps=0
  self.locks={B:(80,1) if busy else (None,0),A:(81,1)};self.actor=81
  self.map(0x70000000);self.map(STOP)
  self.map(FUNC);self.map(0x80ade0d4)
  if patched:self.u.mem_write(0x80ade0d4,PATCH)
  self.map(B);self.u.mem_write(B+0x85,bytes([completed]))
  self.u.reg_write(UC_ARM64_REG_SP,0x70000f00);self.u.reg_write(UC_ARM64_REG_X0,ARG);self.u.reg_write(UC_ARM64_REG_X30,STOP)
  self.u.hook_add(UC_HOOK_MEM_UNMAPPED,self.missing);self.u.hook_add(UC_HOOK_CODE,self.step)
 def map(self,address):
  page=address&~4095
  if page in self.pages:return
  self.u.mem_map(page,4096);self.pages.add(page)
  try:self.u.mem_write(page,guest_read(page,4096))
  except ValueError:pass
 def missing(self,u,access,address,size,value,data):
  for p in range(address&~4095,(address+size+4095)&~4095,4096):self.map(p)
  return True
 def ret(self,value=None):
  if value is not None:self.u.reg_write(UC_ARM64_REG_X0,value)
  self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_X30))
 def step(self,u,address,size,data):
  self.steps+=1
  if address in [0x85bd1e70,0x85bd17d0,0x85bd17e0,0x85bd1b30]:
   arg=u.reg_read(UC_ARM64_REG_X0);self.calls.append((address,arg))
   if address==0x85bd1b30:self.ret();return
   owner,count=self.locks.get(arg,(None,0))
   if address==0x85bd17e0:
    assert owner==self.actor,('unlock without ownership',hex(arg),owner)
    self.locks[arg]=(self.actor if count>1 else None,count-1);self.ret();return
   if owner in [None,self.actor]:self.locks[arg]=(self.actor,count+1);self.ret(1);return
   if address==0x85bd1e70:self.ret(0);return
   self.blocked=True;u.emu_stop()
 def run(self):
  self.u.emu_start(FUNC,STOP,count=50000)
  return {'blocked':self.blocked,'result':None if self.blocked else self.u.reg_read(UC_ARM64_REG_X0),'steps':self.steps,'calls':len(self.calls),'locks':dict(self.locks)}

if __name__=='__main__':
 passed=blocked=0
 for retries in [0,1,5,100,1000]:
  for completed in [0,1]:
   for busy in [False,True]:
    for queued in [False,True]:
     old=Replay(False,busy,completed);new=Replay(True,busy,completed)
     for r in [old,new]:
      r.u.mem_write(B+0x20,struct.pack('<I',retries))
      r.map(0x899373e0);manager=struct.unpack('<Q',r.u.mem_read(0x899373e0,8))[0];r.map(manager+0x1b0)
      r.u.mem_write(manager+0x1b0,struct.pack('<I',int(queued)))
      r.map(0x70002000);r.u.mem_write(0x70002000,struct.pack('<Q',ARG))
      r.u.mem_write(manager+0x1a8,struct.pack('<Q',0x70002000))
     o=old.run();n=new.run()
     if busy:
      assert o['blocked'] and not n['blocked'] and n['result']==0
      assert n['locks'][B]==(80,1) and n['locks'][A]==(81,1);blocked+=1
     else:
      assert o==n
      assert old.u.mem_read(B,0x90)==new.u.mem_read(B,0x90)
      assert n['result']==int(queued and not completed)
     passed+=1
 print(f'PASS {passed} exact ARM64 cases; {blocked} original circular-wait paths reproduced; completed/incomplete, queued/absent, retry counts 0..1000; fast-path calls/instructions/data identical')
