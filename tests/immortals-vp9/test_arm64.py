import contextlib, io, json, pathlib, struct, sys, time
ROOT = pathlib.Path(__file__).resolve().parents[2]
DIAG = ROOT / 'build/pr396-corrections/immortal-freeze'
sys.path.insert(0, str(DIAG / 'python-deps'))
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from patch import ADDRESS, OFFSET, ORIGINAL, BUILD_ID, patch_bytes, ips_bytes
# Extract the SDK lock/unlock and join instructions from the captured process.
# Neither original nor patched test replaces the ARM64 mutex implementation.
saved_argv = sys.argv[:]
sys.argv = [sys.argv[0], sys.argv[1]]
namespace = {}
with contextlib.redirect_stdout(io.StringIO()):
    exec((DIAG/'inspect-guest.py').read_text().split(' sentinel=process+11240')[0], namespace)
read = namespace['guest_read']
assert read(ADDRESS, 36) == ORIGINAL, 'Captured instructions do not match this patch'
assert read(0x80004008, 4) == b'MOD0', 'Main module base mismatch'
sdk = {0x8ab27800: read(0x8ab27800, 96),
       0x8ab2d410: read(0x8ab2d410, 192),
       0x8ab2d500: read(0x8ab2d500, 144)}
THREAD_SIZE, MUTEX, HANDLE = 0x1c8, 0x1a8, 0x3c01aa
OWNER, ORIGINAL_THREAD, SP, TLS, CURRENT = 0x200000, 0x210098, 0x222000, 0x230000, 0x240000
COPY = SP + 8
class Blocked(Exception): pass
class Fixture:
 def __init__(self, patched, finish_chunk, held, tls_cleanup=False, seed=1):
    self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM); self.pages = set(); self.alive = finish_chunk != -1
    self.finish_chunk = finish_chunk; self.calls = []; self.instructions = 0; self.blocked = False
    for addr,size in [(ADDRESS, 64), (0x85bd1610, 32), (0x85bd1c00, 32),
                      (0x8ad602d0,32),(0x8ad60270,32),(0x8ad5f7a0,32),(0x8ad5f7c0,32),
                      (0x8ab7ccb8,32),(OWNER,0x1000),(0x210000,0x1000),
                      (SP-0x1000,0x2000),(TLS,0x1000),(CURRENT,0x1000)]:self.map(addr,size)
    for addr,data in sdk.items():self.map(addr,len(data));self.u.mem_write(addr,data)
    self.u.mem_write(ADDRESS, (patch_bytes() if patched else ORIGINAL) + read(ADDRESS+36,4))
    self.put64(OWNER, ORIGINAL_THREAD - 0x98)
    self.allocation = 0x123400000000 + seed * 4096
    payload = bytearray(((x*17+seed) & 255) for x in range(THREAD_SIZE))
    payload[0x40] = 4; payload[0x41] = int(tls_cleanup)
    payload[MUTEX:MUTEX+4] = struct.pack('<I', HANDLE if held and self.alive else 0)
    payload[0x1b0:0x1b4] = struct.pack('<I', HANDLE)
    payload[0x1c0:0x1c8] = struct.pack('<Q', self.allocation)
    self.u.mem_write(ORIGINAL_THREAD, bytes(payload))
    self.put64(TLS+0x1f8, CURRENT);self.put32(CURRENT+0x1b0,0x1b01d3)
    self.u.reg_write(UC_ARM64_REG_TPIDRRO_EL0,TLS)
    self.u.reg_write(UC_ARM64_REG_X19,OWNER); self.u.reg_write(UC_ARM64_REG_SP,SP)
    self.u.hook_add(UC_HOOK_CODE,self.step)
 def map(self,addr,size):
    for page in range(addr&~4095,(addr+size+4095)&~4095,4096):
     if page not in self.pages:self.u.mem_map(page,4096);self.pages.add(page)
 def put32(self,addr,val):self.u.mem_write(addr,struct.pack('<I',val))
 def put64(self,addr,val):self.u.mem_write(addr,struct.pack('<Q',val))
 def reg(self,index):return self.u.reg_read(UC_ARM64_REG_X0+index)
 def finish(self):
    if self.alive:self.alive=False;self.put32(ORIGINAL_THREAD+MUTEX,0)
 def ret(self):
    # Deliberately clobber caller-saved X8, like the real memcpy can do.
    self.u.reg_write(UC_ARM64_REG_X8,0xdeadbeef)
    self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def step(self,u,addr,size,user):
    self.instructions+=1
    if addr==0x85bd1610:
     dst,src,count = self.reg(0),self.reg(1),self.reg(2)
     assert (dst,src,count)==(COPY,ORIGINAL_THREAD,THREAD_SIZE)
     self.calls.append('copy')
     for chunk,off in enumerate(range(0,count,8)):
      if chunk==self.finish_chunk:self.finish()
      u.mem_write(dst+off,bytes(u.mem_read(src+off,min(8,count-off))))
     if self.finish_chunk==57:self.finish()
     # memcpy returns its destination in X0 (AAPCS64).
     self.ret()
    elif addr==0x85bd1c00:
     self.calls.append('join');self.join_pointer=self.reg(0)
     u.reg_write(UC_ARM64_REG_X1,self.reg(0));u.reg_write(UC_ARM64_REG_X0,0)
     u.reg_write(UC_ARM64_REG_PC,0x8ab27800)
    elif addr==0x8ad602d0:
     assert struct.unpack('<I',u.mem_read(self.reg(1)+0x1b0,4))[0]==HANDLE
     self.finish(); self.ret()
    elif addr==0x8ad5f7a0:u.reg_write(UC_ARM64_REG_PC,0x8ab2d410)
    elif addr==0x8ad5f7c0:u.reg_write(UC_ARM64_REG_PC,0x8ab2d500)
    elif addr==0x8ad60270:self.calls.append('tls_cleanup');self.ret()
    elif addr==0x8ab7ccb8:
     self.blocked=True
     assert self.reg(0)==HANDLE and self.reg(1)==COPY+MUTEX and not self.alive
     raise Blocked('WaitForAddress on copied mutex belonging to exited worker')
    elif addr==0x85bd1c10:
     assert self.reg(0)==COPY and not self.alive
     assert struct.unpack('<I',u.mem_read(COPY+MUTEX,4))[0]==0
     self.calls.append('destroy');u.reg_write(UC_ARM64_REG_X0,0xfeedface);self.ret()
 def run(self):
    try:self.u.emu_start(ADDRESS,ADDRESS+40,count=2000)
    except Blocked:return False
    assert self.u.reg_read(UC_ARM64_REG_PC)==ADDRESS+40
    assert self.reg(0)==self.allocation, 'Original stack allocation lost'
    assert self.u.reg_read(UC_ARM64_REG_X19)==OWNER and self.u.reg_read(UC_ARM64_REG_SP)==SP
    assert self.calls.count('copy')==self.calls.count('join')==self.calls.count('destroy')==1
    return True
start=time.perf_counter(); failures=0; baseline_pass=0; fixed=0; counts={False:[],True:[]}
for held in (False,True):
 for cleanup in (False,True):
  for chunk in range(-1,59):
   for patched in (False,True):
    test=Fixture(patched,chunk,held,cleanup,seed=chunk+3)
    ok=test.run()
    if patched:
     assert ok and test.join_pointer==ORIGINAL_THREAD
     assert test.calls.index('join')<test.calls.index('copy')<test.calls.index('destroy')
     fixed+=1;counts[True].append(test.instructions)
    elif ok:baseline_pass+=1;counts[False].append(test.instructions)
    else:failures+=1
assert failures>0 and baseline_pass>0, 'Missing positive or negative control'
# IPS record must change exactly the tested 36 instructions bytes at NSO-relative + header.
ips=ips_bytes(); offset=int.from_bytes(ips[5:8],'big');length=int.from_bytes(ips[8:10],'big')
assert offset==OFFSET+0x100 and length==36 and ips[10:46]==patch_bytes() and ips[-3:]==b'EOF'
# Compare successful controls: no extra calls, instructions, or copied bytes.
plain=Fixture(False,-1,False);patched=Fixture(True,-1,False)
assert plain.run() and patched.run()
assert patched.instructions==plain.instructions
summary={'fixed_pass':fixed,'baseline_blocked':failures,'baseline_pass':baseline_pass,
 'elapsed_seconds':round(time.perf_counter()-start,3),
 'instruction_count_success':{'original':plain.instructions,'fixed':patched.instructions},
 'copy_bytes':THREAD_SIZE,'join_calls':1,'destroy_calls':1,'patch_bytes':length,
 'build_id':BUILD_ID,'nso_image_offset':hex(OFFSET)}
print(json.dumps(summary,indent=2))
print('PATCH DISASSEMBLY')
for ins in Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(patch_bytes(),ADDRESS):print(f'{ins.address:x}: {ins.mnemonic} {ins.op_str}')
