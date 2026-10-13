"""Build-specific compatibility patch; no game image is modified."""
from pathlib import Path
import struct,sys
BUILD_ID='70F3F6751D73C644BCE904CCB414E35B'.ljust(64,'0')
MAIN_BASE=0x80004000
ADDRESS=0x80ade0d4
ORIGINAL=bytes.fromhex('f6031f2a15e28452')
def patch_bytes():
    # Failed TryLock: return false through the existing cancellation epilogue.
    delta=0x80ade0ac-(ADDRESS+4)
    return struct.pack('<II',0x2a1f03e0,0x14000000|((delta>>2)&0x3ffffff))
def ips_bytes():
    data=patch_bytes()
    # The loader presents the decompressed image with its 0x100-byte NSO header.
    offset=ADDRESS-MAIN_BASE+0x100
    return b'PATCH'+offset.to_bytes(3,'big')+len(data).to_bytes(2,'big')+data+b'EOF'
if __name__=='__main__':
    path=Path(sys.argv[1]);path.mkdir(parents=True,exist_ok=True)
    output=path/(BUILD_ID+'.ips');output.write_bytes(ips_bytes());print(output)
