"""A readable listing of one ENU.SYN function: labels at jump targets, constants resolved.

  python tools/listing.py 0x1013caf0 [end] > build/fn/1013caf0.asm

Memory operands at fixed addresses are shown with their value when they fall in .rdata/.data
(float, double or int). The function ends at the first `ret` after `end`, or at the last `ret` before a
run of int3/padding if no end is given.
"""
import struct
import sys

import capstone

sys.path.insert(0, __file__.rsplit("tools", 1)[0] + "tools")
import disasm  # noqa: E402


def main():
    d, secs = disasm.load()
    start = int(sys.argv[1], 16)
    end = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    off = disasm.to_off(secs, start)
    ins = []
    for x in md.disasm(d[off:off + 0x10000], start):
        ins.append(x)
        if x.mnemonic == "ret" and (end is None or x.address >= end):
            nxt = d[disasm.to_off(secs, x.address + x.size):][:1]
            if end is not None or nxt in (b"\xcc", b"\x90"):
                break
    targets = set()
    for x in ins:
        if x.mnemonic.startswith("j") and x.op_str.startswith("0x"):
            targets.add(int(x.op_str, 16))

    def val(addr, size):
        o = disasm.to_off(secs, addr)
        if o is None:
            return ""
        b = d[o:o + size]
        if size == 4:
            return "  ; =%r f / %d" % (struct.unpack("<f", b)[0], struct.unpack("<i", b)[0])
        if size == 8:
            return "  ; =%r d" % struct.unpack("<d", b)[0]
        if size == 10:
            return "  ; (tbyte)"
        return ""

    for x in ins:
        if x.address in targets:
            print("L_%x:" % x.address)
        note = ""
        for op in x.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp > 0x10000000:
                note = val(op.mem.disp, op.size)
        print("    %-8x %-8s %s%s" % (x.address, x.mnemonic, x.op_str, note))


main()
