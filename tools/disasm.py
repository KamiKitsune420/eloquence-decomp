"""Disassemble a range of the engine, by Ghidra address.

  python tools/dis.py 0x10078340 0x100783e0        a range
  python tools/dis.py 0x100783be --back 0x120      that address and what leads to it
"""
import argparse
import struct

import capstone

DLL = __file__.rsplit("tools", 1)[0] + "pkg/ENU.SYN"
BASE = 0x10000000


def load():
    d = open(DLL, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    base = pe + 24 + opt
    secs = []
    for i in range(nsec):
        o = base + 40 * i
        nm = d[o:o + 8].rstrip(b"\0").decode()
        vsz, va, rsz, ra = struct.unpack_from("<IIII", d, o + 8)
        secs.append((nm, va, vsz, ra, rsz))
    return d, secs


def to_off(secs, addr):
    rva = addr - BASE
    for nm, va, vsz, ra, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return ra + (rva - va)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start", type=lambda s: int(s, 0))
    ap.add_argument("end", nargs="?", type=lambda s: int(s, 0))
    ap.add_argument("--back", type=lambda s: int(s, 0), default=0)
    a = ap.parse_args()
    d, secs = load()
    start = a.start - a.back
    end = a.end if a.end else a.start + 0x40
    off = to_off(secs, start)
    if off is None:
        raise SystemExit("address not in any section")
    code = d[off:off + (end - start)]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    for ins in md.disasm(code, start):
        mark = "  <<<" if ins.address == a.start else ""
        print("  %08x  %-22s %s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, mark))


if __name__ == "__main__":
    main()
