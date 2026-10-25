"""Reads decrypted Switch containers (NSP/PFS0, XCI/HFS0, NCA) and returns the ExeFS files.

Only decrypted dumps are supported: NCA headers and sections must already be plaintext.
"""
import struct


class Slice:
    """A window [offset, offset + size) inside an open file."""

    def __init__(self, f, offset, size):
        self.f, self.offset, self.size = f, offset, size

    def read(self, off, n):
        if off < 0 or off + n > self.size:
            raise ValueError(f"read out of bounds: {off:#x}+{n:#x} > {self.size:#x}")
        self.f.seek(self.offset + off)
        data = self.f.read(n)
        if len(data) != n:
            raise ValueError("truncated read")
        return data

    def sub(self, off, n):
        if off < 0 or off + n > self.size:
            raise ValueError("sub-slice out of bounds")
        return Slice(self.f, self.offset + off, n)


def parse_fs0(s):
    """PFS0 or HFS0: returns {name: Slice}."""
    magic = s.read(0, 4)
    if magic not in (b"PFS0", b"HFS0"):
        raise ValueError(f"not a PFS0/HFS0: {magic!r}")
    count, strtab_size = struct.unpack("<II", s.read(4, 8))
    entry_size = 0x18 if magic == b"PFS0" else 0x40
    entries = s.read(0x10, count * entry_size)
    strtab = s.read(0x10 + count * entry_size, strtab_size)
    data_start = 0x10 + count * entry_size + strtab_size
    out = {}
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", entries, i * entry_size)
        name = strtab[name_off:strtab.index(b"\0", name_off)].decode()
        out[name] = s.sub(data_start + off, size)
    return out


NCA_PROGRAM = 0


def nca_info(s):
    """Returns (content_type, program_id, [section slices with fs type]) of a plaintext NCA."""
    hdr = s.read(0, 0xC00)
    if hdr[0x200:0x204] not in (b"NCA3", b"NCA2", b"DNCA"):
        raise ValueError("NCA header not plaintext")
    content_type = hdr[0x205]
    program_id = struct.unpack_from("<Q", hdr, 0x210)[0]
    sections = []
    for i in range(4):
        start, end = struct.unpack_from("<II", hdr, 0x240 + i * 0x10)
        if end <= start:
            continue
        fsh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]
        fs_type, hash_type, crypto_type = fsh[2], fsh[3], fsh[4]
        sections.append((i, fs_type, hash_type, crypto_type, fsh, s.sub(start * 0x200, (end - start) * 0x200)))
    return content_type, program_id, sections


def exefs_from_nca(s):
    """Returns the ExeFS {name: Slice} of a program NCA, or None."""
    content_type, program_id, sections = nca_info(s)
    if content_type != NCA_PROGRAM:
        return None
    for idx, fs_type, hash_type, crypto_type, fsh, sec in sections:
        if fs_type != 1:  # PFS0
            continue
        # Hierarchical SHA-256: the PFS0 is the second hash-data region.
        pfs_off, pfs_size = struct.unpack_from("<QQ", fsh, 0x8 + 0x38)
        try:
            fs = parse_fs0(sec.sub(pfs_off, pfs_size))
        except ValueError:
            continue
        if "main.npdm" in fs or "main" in fs:
            return program_id, fs
    return None


def exefs_from_container(path):
    """Yields (program_id, {name: Slice}) for every ExeFS found in an NSP or XCI."""
    f = open(path, "rb")
    import os
    size = os.fstat(f.fileno()).st_size
    root = Slice(f, 0, size)
    head = root.read(0, 4)
    if head == b"PFS0":
        files = parse_fs0(root)
    else:
        # XCI: root HFS0 at the partition offset of the cartridge header.
        if root.read(0x100, 4) not in (b"HEAD", b"DXCI"):
            raise ValueError("unknown container")
        # 0x138 is the size of the root HFS0 header only; its data runs to the end of the file.
        hfs_off = struct.unpack("<Q", root.read(0x130, 8))[0]
        parts = parse_fs0(root.sub(hfs_off, size - hfs_off))
        files = parse_fs0(parts["secure"])
    for name, sl in files.items():
        if not name.endswith(".nca"):
            continue
        try:
            r = exefs_from_nca(sl)
        except ValueError:
            continue
        if r:
            yield r
