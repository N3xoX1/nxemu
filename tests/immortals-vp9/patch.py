"""Generate the build-specific IPS; no game image is written."""
import pathlib, struct, sys
BUILD_ID = '70F3F6751D73C644BCE904CCB414E35B'.ljust(64, '0')
MAIN_BASE = 0x80004000
ADDRESS = 0x804b8240
OFFSET = ADDRESS - MAIN_BASE
ORIGINAL = bytes.fromhex('680240f90161029102398052e0230091f0645c95e02300916a665c95e02300916c665c95')
def bl(address, target):
    delta = target - address
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
    return 0x94000000 | ((delta >> 2) & 0x3ffffff)
def patch_bytes():
    # Join the original ThreadType first; only copy it after its worker has exited.
    words = [0xf9400268, 0x91026100, bl(ADDRESS + 8, 0x85bd1c00),
             0xf9400268, 0x91026101, 0x910023e0, 0x52803902,
             bl(ADDRESS + 28, 0x85bd1610), bl(ADDRESS + 32, 0x85bd1c10)]
    return struct.pack('<9I', *words)
def ips_bytes():
    data = patch_bytes()
    # NXEmu passes the decompressed image with its 0x100-byte NSO header to PatchIPS.
    return b'PATCH' + (OFFSET + 0x100).to_bytes(3, 'big') + len(data).to_bytes(2, 'big') + data + b'EOF'
if __name__ == '__main__':
    output = pathlib.Path(sys.argv[1]); output.mkdir(parents=True, exist_ok=True)
    path = output / (BUILD_ID + '.ips'); path.write_bytes(ips_bytes()); print(path)
