"""What the callers of an ENU.SYN function read of the registers it leaves: for each direct call site,
the paths after the call are followed (branches both ways, up to a limit) until each of al / ah / the
upper 16 bits of eax, and edx / ecx, is overwritten or read. A `ret` with a part still holding the
callee's value, or a tail `jmp` to the function, counts as a read (the value goes to the caller's
callers). A call to another function ends the path (it sets eax/ecx/edx itself).

  python tools/retuse.py 0x10131790 0x10132b80 ...      one line per function
  python tools/retuse.py --range 0x10130e80 0x10136000  every function Ghidra found in the range

Output: address, number of call sites, and the parts read somewhere: al ah hi edx ecx (or "none").
Used to justify comparing only al (bool results) or nothing (void) in src/difftest.c (src/ported.h flags).
"""
import os
import re
import sys

import capstone
from capstone import x86_const as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disasm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

d, secs = disasm.load()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
TEXT = [s for s in secs if s[0] == ".text"][0]
TLO, THI = 0x10000000 + TEXT[1], 0x10000000 + TEXT[1] + TEXT[2]

_cache = {}


def insn(a):
    if a in _cache:
        return _cache[a]
    off = disasm.to_off(secs, a)
    x = next(md.disasm(d[off:off + 16], a), None)
    _cache[a] = x
    return x


# register -> parts: al=1 ah=2 hi=4 (eax), edx=8, ecx=16
PARTS = {X.X86_REG_AL: 1, X.X86_REG_AH: 2, X.X86_REG_AX: 3, X.X86_REG_EAX: 7,
         X.X86_REG_DL: 8, X.X86_REG_DH: 8, X.X86_REG_DX: 8, X.X86_REG_EDX: 8,
         X.X86_REG_CL: 16, X.X86_REG_CH: 16, X.X86_REG_CX: 16, X.X86_REG_ECX: 16}
# a write of these kills: 8-bit and 16-bit writes keep the rest
KILLS = {X.X86_REG_AL: 1, X.X86_REG_AH: 2, X.X86_REG_AX: 3, X.X86_REG_EAX: 7,
         X.X86_REG_EDX: 8, X.X86_REG_ECX: 16}


def uses(x):
    """(parts read, parts written) by one instruction"""
    try:
        rd, wr = x.regs_access()
    except capstone.CsError:
        return 31, 0
    r = 0
    for g in rd:
        r |= PARTS.get(g, 0)
    w = 0
    for g in wr:
        w |= KILLS.get(g, 0)
    m = x.mnemonic
    # idioms that read nothing: xor r, r / sub r, r
    if m in ("xor", "sub") and len(x.operands) == 2 and x.operands[0].type == X.X86_OP_REG \
            and x.operands[1].type == X.X86_OP_REG and x.operands[0].reg == x.operands[1].reg:
        r &= ~PARTS.get(x.operands[0].reg, 0)
    # movzx/movsx eax, al reads al only (regs_access already says so)
    return r, w


def follow(start, live, limit=60):
    """parts read on any path from start while `live` parts still hold the callee's value"""
    seen = set()
    stack = [(start, live, 0)]
    found = 0
    while stack:
        a, lv, n = stack.pop()
        while lv and n < limit:
            if (a, lv) in seen:
                break
            seen.add((a, lv))
            x = insn(a)
            if x is None:
                return 31
            m = x.mnemonic
            r, w = uses(x)
            found |= r & lv
            if m == "call":
                break
            if m in ("ret", "retn"):
                found |= lv & ret_reads(func_of(a))   # returned: what the caller's callers read
                break
            lv &= ~w
            nxt = a + x.size
            if m == "jmp":
                if x.operands[0].type == X.X86_OP_IMM:
                    a = x.operands[0].imm
                    n += 1
                    continue
                found |= lv          # indirect jump (switch): give up
                break
            if m.startswith("j") or m.startswith("loop"):
                if x.operands[0].type == X.X86_OP_IMM:
                    stack.append((x.operands[0].imm, lv, n + 1))
            a = nxt
            n += 1
        else:
            if lv and n >= limit:
                found |= lv          # path too long: assume read
    return found


ENTRIES = sorted(int(m, 16) for m in re.findall(
    r"(?m)^// \S+ @ ([0-9a-f]{8})$",
    open(os.path.join(ROOT, "decomp", "ENU.SYN", "_all.c"), encoding="utf-8", errors="replace").read()))


def func_of(a):
    import bisect
    i = bisect.bisect_right(ENTRIES, a) - 1
    return ENTRIES[i] if i >= 0 else None


_ret = {}
_sites = None


def ret_reads(f):
    """parts of eax/edx/ecx that the callers of f read of what f returns (recursive, memoized; a cycle
    counts as everything)"""
    if f is None:
        return 31
    if f in _ret:
        return _ret[f]
    _ret[f] = 31                      # while being computed
    p = 0
    for at, m, ret in all_sites().get(f, []):
        if m == "jmp":
            p |= ret_reads(func_of(at))
        else:
            p |= follow(ret, 31)
    if not all_sites().get(f):
        p = 31                        # no direct callers: called through a pointer
    _ret[f] = p
    return p


def all_sites():
    global _sites
    if _sites is None:
        nm, va, vsz, ra, rsz = TEXT
        md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md2.skipdata = True
        _sites = {}
        for x in md2.disasm(d[ra:ra + vsz], 0x10000000 + va):
            if x.mnemonic in ("call", "jmp") and x.op_str.startswith("0x"):
                t = int(x.op_str, 16)
                if t in ENTRIES_SET:
                    _sites.setdefault(t, []).append((x.address, x.mnemonic, x.address + x.size))
    return _sites


ENTRIES_SET = set(ENTRIES)


def scan_calls(targets):
    nm, va, vsz, ra, rsz = TEXT
    md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md2.skipdata = True
    sites = {t: [] for t in targets}
    for x in md2.disasm(d[ra:ra + vsz], 0x10000000 + va):
        if x.mnemonic in ("call", "jmp") and x.op_str.startswith("0x"):
            t = int(x.op_str, 16)
            if t in sites:
                sites[t].append((x.address, x.mnemonic, x.address + x.size))
    return sites


def names(p):
    out = [n for n, b in (("al", 1), ("ah", 2), ("hi", 4), ("edx", 8), ("ecx", 16)) if p & b]
    return " ".join(out) if out else "none"


def main():
    args = sys.argv[1:]
    if args and args[0] == "--range":
        lo, hi = int(args[1], 16), int(args[2], 16)
        src = open(os.path.join(ROOT, "decomp", "ENU.SYN", "_all.c"), encoding="utf-8", errors="replace").read()
        targets = sorted(a for a in (int(m, 16) for m in re.findall(r"(?m)^// \S+ @ ([0-9a-f]{8})$", src))
                         if lo <= a < hi)
    else:
        targets = [int(a, 16) for a in args]
    sys.setrecursionlimit(100000)
    sites = all_sites()
    for t in targets:
        sites.setdefault(t, [])
        tails = sum(1 for s in sites[t] if s[1] == "jmp")
        p = ret_reads(t)
        print("%08x %4d sites%s  reads: %s" % (t, len(sites[t]), " (%d tail jumps)" % tails if tails else "", names(p)))


main()
