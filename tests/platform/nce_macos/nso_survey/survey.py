"""Static survey of real Switch modules for the macOS NCE.

For every game given on the command line, reads the ExeFS modules (rtld, main, subsdk*, sdk) and
measures what the NCE on 16 KiB host pages must handle:
  - the 16 KiB host pages whose four 4 KiB guest pages do not share one permission ("split"),
    with the NXEmu layout (modules packed one after the other, 4 KiB aligned) and with module
    bases aligned to 16 KiB;
  - the guest uses of x18 (cleared by Darwin on every kernel return);
  - system register accesses (MRS/MSR), SVC, cache maintenance and exclusives.

Usage: survey.py [--json out.json] game1 [game2 ...]
"""
import argparse
import json
import os
import struct
import sys

import capstone
import lz4.block
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from containers import exefs_from_container  # noqa: E402

GUEST_PAGE = 0x1000
HOST_PAGE = 0x4000
# NXEmu load order (deconstructed_rom_directory.cpp).
MODULE_ORDER = ["rtld", "main"] + [f"subsdk{i}" for i in range(10)] + ["sdk"]

R, RW, RX = "R", "RW", "RX"

SYSREGS = {
    (3, 3, 13, 0, 2): "TPIDR_EL0",
    (3, 3, 13, 0, 3): "TPIDRRO_EL0",
    (3, 3, 14, 0, 0): "CNTFRQ_EL0",
    (3, 3, 14, 0, 1): "CNTPCT_EL0",
    (3, 3, 14, 0, 2): "CNTVCT_EL0",
    (3, 3, 0, 0, 1): "CTR_EL0",
    (3, 3, 0, 0, 7): "DCZID_EL0",
    (3, 3, 4, 4, 0): "FPCR",
    (3, 3, 4, 4, 1): "FPSR",
    (3, 3, 4, 2, 0): "NZCV",
    (3, 0, 0, 0, 5): "MPIDR_EL1",
}


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def load_nso(sl):
    """Returns dict with segments [(mem_off, bytes)] and bss size, or None if unsupported."""
    hdr = sl.read(0, 0x100)
    if hdr[:4] != b"NSO0":
        return None
    flags = struct.unpack_from("<I", hdr, 0xC)[0]
    segs = [struct.unpack_from("<IIII", hdr, 0x10 + i * 0x10) for i in range(3)]
    csizes = struct.unpack_from("<III", hdr, 0x60)
    if (flags >> 7) & 1:
        return {"unsupported": "ZBIC"}
    out = []
    for i, (foff, mem, size, _extra) in enumerate(segs):
        compressed = (flags >> i) & 1
        raw = sl.read(foff, csizes[i] if compressed else size)
        data = lz4.block.decompress(raw, uncompressed_size=size) if compressed else raw
        if len(data) != size:
            raise ValueError("segment size mismatch")
        out.append((mem, data))
    return {"segments": out, "bss": segs[2][3]}


def guest_pages(nso):
    """Per 4 KiB guest page permission of one module image, as NXEmu maps it."""
    (t_off, text), (r_off, ro), (d_off, data) = nso["segments"]
    image_end = align_up(d_off + len(data) + nso["bss"], GUEST_PAGE)
    perms = [None] * (image_end // GUEST_PAGE)
    for off, size, perm in ((t_off, len(text), RX), (r_off, len(ro), R), (d_off, len(data) + nso["bss"], RW)):
        for p in range(off // GUEST_PAGE, align_up(off + size, GUEST_PAGE) // GUEST_PAGE):
            perms[p] = perm
    return perms, image_end


def host_page_survey(modules, align_bases):
    """Lays the modules out like NXEmu and classifies every 16 KiB host page.

    Returns {combo: count} for split pages, the number of direct pages, and per-combo byte counts
    of the 4 KiB guest pages that live in split pages.
    """
    pages = {}  # guest page index -> (perm, module)
    base = 0
    for name, nso in modules:
        if align_bases:
            base = align_up(base, HOST_PAGE)
        perms, size = guest_pages(nso)
        for i, perm in enumerate(perms):
            pages[base // GUEST_PAGE + i] = (perm, name)
        base += size
    end = align_up(base, HOST_PAGE)
    direct, split, detail = 0, {}, []
    for hp in range(0, end // GUEST_PAGE, HOST_PAGE // GUEST_PAGE):
        quad = [pages.get(hp + i, (None, None)) for i in range(4)]
        perms = [q[0] for q in quad]
        if len(set(perms)) == 1:
            direct += 1
            continue
        combo = "|".join(p or "--" for p in perms)
        split[combo] = split.get(combo, 0) + 1
        detail.append({"addr": hp * GUEST_PAGE, "perms": perms, "modules": [q[1] for q in quad]})
    return {"direct": direct, "split": split, "split_pages": detail}


def scan_text(text):
    """Counts the instruction classes the macOS NCE has to rewrite or emulate."""
    words = np.frombuffer(text[: len(text) & ~3], dtype="<u4")
    res = {"words": int(words.size)}

    res["svc"] = int(np.count_nonzero((words & 0xFFE0001F) == 0xD4000001))
    res["brk"] = int(np.count_nonzero((words & 0xFFE0001F) == 0xD4200000))
    # Load/store exclusive and ordered (bits 29:24 = 001000): o2 (bit 23) = 0 is LDXR/STXR, 1 is LDAR/STLR.
    excl_class = (words & 0x3F000000) == 0x08000000
    res["exclusive"] = int(np.count_nonzero(excl_class & ((words & 0x00800000) == 0)))
    res["ordered"] = int(np.count_nonzero(excl_class & ((words & 0x00800000) != 0)))
    # LSE atomics (ARMv8.1, absent from the Tegra X1): LDADD..SWP and CAS.
    res["lse"] = int(np.count_nonzero(((words & 0x3B200C00) == 0x38200000) | ((words & 0x3FA07C00) == 0x08A07C00)))
    # SYS (DC/IC/TLBI): 1101 0101 0000 1 op1 CRn CRm op2 Rt
    sys_mask = (words & 0xFFF80000) == 0xD5080000
    sysregs = {}
    sys_ops = {}
    for w in words[sys_mask]:
        op1, crn, crm, op2 = (int(w) >> 16) & 7, (int(w) >> 12) & 15, (int(w) >> 8) & 15, (int(w) >> 5) & 7
        key = {(3, 7, 4, 1): "DC ZVA", (3, 7, 14, 1): "DC CIVAC", (3, 7, 10, 1): "DC CVAC",
               (3, 7, 11, 1): "DC CVAU", (3, 7, 5, 1): "IC IVAU", (0, 7, 5, 0): "IC IALLU",
               (0, 7, 6, 1): "DC IVAC"}.get((op1, crn, crm, op2), f"SYS {op1},{crn},{crm},{op2}")
        sys_ops[key] = sys_ops.get(key, 0) + 1
    # MRS (L=1) and MSR register (L=0): 1101 0101 00 L 1 o0 op1 CRn CRm op2 Rt
    for is_mrs, pattern in ((True, 0xD5300000), (False, 0xD5100000)):
        sel = words[(words & 0xFFF00000) == pattern]
        for w in sel:
            w = int(w)
            key = ((w >> 19) & 1) + 2, (w >> 16) & 7, (w >> 12) & 15, (w >> 8) & 15, (w >> 5) & 7
            name = ("MRS " if is_mrs else "MSR ") + SYSREGS.get(key, "S%d_%d_C%d_C%d_%d" % key)
            sysregs[name] = sysregs.get(name, 0) + 1
    res["sysreg"] = sysregs
    res["sys_ops"] = sys_ops
    # MSR immediate (DAIF, SPSel...) and hints other than NOP are left to the patcher's own check.

    # x18: any 5-bit register field equal to 18, confirmed by a disassembler.
    cand = np.zeros(words.size, dtype=bool)
    for shift in (0, 5, 10, 16):
        cand |= ((words >> shift) & 0x1F) == 18
    idx = np.nonzero(cand)[0]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    x18_regs = {capstone.arm64.ARM64_REG_X18, capstone.arm64.ARM64_REG_W18}
    reads = writes = total = 0
    funcs = set()
    mnemonics = {}
    for i in idx:
        insn = next(md.disasm(text[i * 4:i * 4 + 4], i * 4), None)
        if insn is None:
            continue
        rd, wr = insn.regs_access()
        r18, w18 = bool(x18_regs & set(rd)), bool(x18_regs & set(wr))
        if not (r18 or w18):
            continue
        total += 1
        reads += r18
        writes += w18
        funcs.add(i * 4 >> 12)
        mnemonics[insn.mnemonic] = mnemonics.get(insn.mnemonic, 0) + 1
    res["x18"] = {"insns": total, "reads": reads, "writes": writes, "pages_4k": len(funcs),
                  "top": dict(sorted(mnemonics.items(), key=lambda kv: -kv[1])[:8])}
    return res


def survey_game(path):
    games = []
    seen = set()
    for program_id, fs in exefs_from_container(path):
        modules = []
        for name in MODULE_ORDER:
            if name not in fs:
                continue
            nso = load_nso(fs[name])
            if nso is None or "unsupported" in nso:
                modules.append((name, nso))
                continue
            modules.append((name, nso))
        key = tuple(len(n["segments"][0][1]) for _, n in modules if n and "segments" in n)
        if key in seen:
            continue
        seen.add(key)
        loadable = [(n, m) for n, m in modules if m and "segments" in m]
        entry = {"container": os.path.basename(path), "program_id": f"{program_id:016x}", "modules": []}
        for name, nso in modules:
            if not nso or "segments" not in nso:
                entry["modules"].append({"name": name, "unsupported": (nso or {}).get("unsupported", "?")})
                continue
            (t_off, text), (r_off, ro), (d_off, data) = nso["segments"]
            m = {"name": name, "text": [t_off, len(text)], "ro": [r_off, len(ro)], "data": [d_off, len(data)],
                 "bss": nso["bss"]}
            m.update(scan_text(text))
            entry["modules"].append(m)
        entry["layout_nxemu"] = host_page_survey(loadable, align_bases=False)
        entry["layout_aligned"] = host_page_survey(loadable, align_bases=True)
        games.append(entry)
    return games


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("games", nargs="+")
    args = ap.parse_args()
    results = []
    for g in args.games:
        try:
            r = survey_game(g)
        except Exception as e:  # report and continue with the next game
            print(f"{os.path.basename(g)}: erreur {e}", file=sys.stderr)
            continue
        for e in r:
            results.append(e)
            mods = e["modules"]
            x18 = sum(m.get("x18", {}).get("insns", 0) for m in mods)
            words = sum(m.get("words", 0) for m in mods)
            print(f"{e['container'][:50]:50} {e['program_id']} code={words * 4 / 2**20:6.1f} MiB "
                  f"x18={x18:6d} split(nxemu)={e['layout_nxemu']['split']} "
                  f"split(aligned)={e['layout_aligned']['split']}", flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
