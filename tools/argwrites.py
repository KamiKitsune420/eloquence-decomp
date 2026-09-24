"""Which ENU.SYN functions write into their own argument slots (compilers reuse them as locals), or pass
the address of one: a port has to do the same, since the slots are in the caller's frame.

  python tools/argwrites.py 0x10135d00 ...          or  --range LO HI

Follows esp through the function (push/pop/sub/add/call cleanup; branch targets inherit the delta) and
reports stores to [esp+X] / lea of [esp+X] at or above the first argument, and uses of ebp frames.
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


def insns(start, end):
    off = disasm.to_off(secs, start)
    out = []
    for x in md.disasm(d[off:off + (end - start)], start):
        if x.address >= end:
            break
        out.append(x)
    return out


def scan(start, end):
    hits = []
    delta = 0                       # bytes pushed since entry (esp = entry - delta)
    at = {}
    ebp_frame = False
    for x in insns(start, end):
        if x.address in at:
            delta = at[x.address]
        m = x.mnemonic
        ops = x.operands
        for i, o in enumerate(ops):
            if o.type == X.X86_OP_MEM and o.mem.base == X.X86_REG_ESP and o.mem.index == 0:
                off = o.mem.disp - delta          # relative to the entry esp
                if off >= 4:
                    written = (i == 0 and m not in ("cmp", "test", "push")) or m == "lea"
                    if written or m == "lea":
                        hits.append("%08x %s %s  (arg slot +%d)" % (x.address, m, x.op_str, off))
            if o.type == X.X86_OP_MEM and o.mem.base == X.X86_REG_EBP and ebp_frame:
                if o.mem.disp >= 8 and ((i == 0 and m not in ("cmp", "test", "push")) or m == "lea"):
                    hits.append("%08x %s %s  (ebp arg +%d)" % (x.address, m, x.op_str, o.mem.disp - 4))
        if m == "push":
            delta += 4
        elif m == "pop":
            delta -= 4
        elif m in ("sub", "add") and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP \
                and ops[1].type == X.X86_OP_IMM:
            delta += ops[1].imm if m == "sub" else -ops[1].imm
        elif m == "mov" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_EBP \
                and ops[1].type == X.X86_OP_REG and ops[1].reg == X.X86_REG_ESP:
            ebp_frame = True
        if m.startswith("j") and ops and ops[0].type == X.X86_OP_IMM:
            at.setdefault(ops[0].imm, delta)
        if m in ("ret", "jmp"):
            delta = None if False else delta
    return hits


def main():
    src = open(os.path.join(ROOT, "decomp", "ENU.SYN", "_all.c"), encoding="utf-8", errors="replace").read()
    ents = sorted(int(m, 16) for m in re.findall(r"(?m)^// \S+ @ ([0-9a-f]{8})$", src))
    a = sys.argv[1:]
    if a and a[0] == "--range":
        targets = [e for e in ents if int(a[1], 16) <= e < int(a[2], 16)]
    else:
        targets = [int(x, 16) for x in a]
    for t in targets:
        i = ents.index(t)
        end = ents[i + 1] if i + 1 < len(ents) else t + 0x1000
        h = scan(t, end)
        if h:
            print("%08x" % t)
            for l in h:
                print("   ", l)


main()
