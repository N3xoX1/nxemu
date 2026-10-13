import mmap,struct,pathlib,sys,bisect,re
file=open(sys.argv[1],'rb'); dump=mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ)
def unpack(fmt,offset):return struct.unpack_from(fmt,dump,offset)
streams={}
count,directory=unpack('<II',8)
for i in range(count):
 kind,size,rva=unpack('<III',directory+i*12);streams[kind]=(rva,size)
ranges=[]
if 9 in streams:
 pos=streams[9][0];count,offset=unpack('<QQ',pos)
 for i in range(count):
  start,size=unpack('<QQ',pos+16+i*16);ranges.append((start,start+size,offset));offset+=size
elif 5 in streams:
 pos=streams[5][0];count=unpack('<I',pos)[0]
 for i in range(count):
  start,size,offset=unpack('<QII',pos+4+i*16);ranges.append((start,start+size,offset))
ranges.sort();starts=[r[0] for r in ranges]
def read(address,size):
 ix=bisect.bisect_right(starts,address)-1
 if ix<0 or address+size>ranges[ix][1]:raise ValueError(f'Missing {address:x}+{size:x}')
 return dump[ranges[ix][2]+address-ranges[ix][0]:ranges[ix][2]+address-ranges[ix][0]+size]
def u64(address):return struct.unpack('<Q',read(address,8))[0]
def u32(address):return struct.unpack('<I',read(address,4))[0]

process=0x180ba9988
memory_impl=u64(process+11688+16);page_table=u64(memory_impl+8);pointers=u64(page_table+8)
def guest_read(address,size):
 base=u64(pointers+(address>>12)*8)&~3
 return read((base+address)&0xffffffffffffffff,size)

from patch import ADDRESS,ORIGINAL
assert guest_read(ADDRESS,len(ORIGINAL))==ORIGINAL,'This fixture requires the documented loading-hang snapshot'
