"""Every call/jmp to the given functions in ENU.SYN, with the instructions around it.
  python tools/callsites.py 0x1013caf0 [...]"""
import sys, capstone
sys.path.insert(0, __file__.rsplit("tools", 1)[0] + "tools")
import disasm
d, secs = disasm.load()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
nm, va, vsz, ra, rsz = [s for s in secs if s[0] == ".text"][0]
ins = list(md.disasm(d[ra:ra + vsz], 0x10000000 + va))
want = {int(a, 16) for a in sys.argv[1:]}
for i, x in enumerate(ins):
    if x.mnemonic in ("call", "jmp") and x.op_str.startswith("0x") and int(x.op_str, 16) in want:
        pre = "; ".join("%s %s" % (p.mnemonic, p.op_str) for p in ins[max(0, i - 6):i])
        post = "; ".join("%s %s" % (p.mnemonic, p.op_str) for p in ins[i + 1:i + 3])
        print("%x %s %s | %s | %s" % (x.address, x.mnemonic, x.op_str, pre, post))
