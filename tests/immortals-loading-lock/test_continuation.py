"""Compare captured caller/completion cleanup with original serial execution."""
from test_arm64 import *
class Continuation(Replay):
 def step(self,u,address,size,data):
  if address in [0x85bd1b00,0x80b060e0]:
   self.calls.append((address,u.reg_read(UC_ARM64_REG_X0)));self.ret();return
  super().step(u,address,size,data)
 def setup_frame(self):
  self.u.reg_write(UC_ARM64_REG_SP,0x70000e00)
  self.u.mem_write(0x70000e50,struct.pack('<QQ',0,STOP))
 def caller(self):
  self.setup_frame();self.locks[A]=(None,0)
  manager=0x1185f3c6c0
  for reg,value in [(UC_ARM64_REG_X20,manager),(UC_ARM64_REG_X24,manager+0x30),(UC_ARM64_REG_X25,0xffffffffffffffff),(UC_ARM64_REG_X19,0),(UC_ARM64_REG_X23,0)]:self.u.reg_write(reg,value)
  self.u.emu_start(0x80af01d0,STOP,count=20000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==STOP and not self.blocked
  assert self.locks[A]==(None,0)
  assert self.u.mem_read(manager+0x30,16)==bytes(16)
  print('CALLER completed, returned',self.u.reg_read(UC_ARM64_REG_X0),'A released; B ownership preserved:',self.locks[B])
 def callback(self):
  self.actor=80;self.setup_frame();self.u.reg_write(UC_ARM64_REG_X30,STOP)
  self.u.reg_write(UC_ARM64_REG_X1,ARG);self.u.reg_write(UC_ARM64_REG_X2,0)
  self.u.reg_write(UC_ARM64_REG_TPIDR_EL0,0x1185b64420)
  self.u.emu_start(0x80af0730,STOP,count=100000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==STOP and not self.blocked
  assert self.locks[A]==(None,0)
  assert self.u.mem_read(0x1185fb4d20+0x38,8)==bytes(8)
  print('CALLBACK completed, A released, actual job reference cleared')

run=Continuation(True,True,1);run.caller();run.callback()
reference=Continuation(False,True,1);reference.locks[A]=(None,0);reference.callback();reference.locks[B]=(None,0);reference.actor=81;reference.caller()
for address,size in [(0x1185f3c6c0,0x1e0),(0x1185fb4d20,0x60),(ARG,0x20)]:
 run.map(address);reference.map(address)
 assert run.u.mem_read(address,size)==reference.u.mem_read(address,size),('bookkeeping differs',hex(address))
print('PASS original serial completion vs corrected interleaving: manager, job and callback record data identical; no forced release of another owner')
