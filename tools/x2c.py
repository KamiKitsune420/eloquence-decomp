"""x2c - recompile 32-bit x86 machine code (MSVC-compiled) into C that runs on src/x86rt.h.

  python tools/x2c.py --tree 0x1013caf0 [...] -o src/gen_x86.c [--image pkg/ENU.SYN]

Every guest function becomes `void f_XXXXXXXX(cpu *c)`. Registers, flags, the x87 stack and memory are the
runtime's; each instruction becomes C with exactly its effect, so what the recompiled code computes is what
the chip computed (the x87 in exact 80-bit arithmetic, see src/fx80.h). Nothing here depends on a
decompiler.

  * functions: entries given, plus (with --tree) everything they call directly; Ghidra's function list
    (decomp/*/_all.c headers) tells which jump targets are other functions (tail calls)
  * jump tables `jmp [reg*4 + table]` are read from the image and become switches
  * calls to import thunks (`jmp [IAT]`) and `call [IAT]` go to the runtime's imp_<name>
  * other indirect calls go through x86_icall (a table of every recompiled function)
  * x87 instructions are decoded from their bytes (D8-DF + modrm) per the Intel manual, not from the
    disassembler's mnemonics
"""
import argparse
import os
import re
import struct
import sys

import capstone
from capstone import x86 as X

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ------------------------------------------------------------------------------------------------ image


class Image:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        pe = struct.unpack_from("<I", d, 0x3c)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.size = struct.unpack_from("<I", d, pe + 24 + 56)[0]
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + opt + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode()
            vsz, va, rsz, ra = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.secs.append((name, va, vsz, ra, rsz, chars))
        self.text = [s for s in self.secs if s[0] == ".text"][0]
        imp_rva, imp_size = struct.unpack_from("<II", d, pe + 24 + 96 + 8)
        self.imports = {}           # IAT slot address -> (dll, name)
        o = self.off(self.base + imp_rva)
        while True:
            ilt, ts, fw, name, iat = struct.unpack_from("<IIIII", d, o)
            if not name:
                break
            dll = self.cstr(self.base + name)
            j = 0
            while True:
                e = struct.unpack_from("<I", d, self.off(self.base + (ilt or iat)) + 4 * j)[0]
                if not e:
                    break
                fn = ("#%d" % (e & 0xffff)) if e & 0x80000000 else self.cstr(self.base + e + 2)
                self.imports[self.base + iat + 4 * j] = (dll, fn)
                j += 1
            o += 20

    def off(self, addr):
        rva = addr - self.base
        for name, va, vsz, ra, rsz, ch in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return ra + (rva - va) if rva - va < rsz else None
        return None

    def cstr(self, addr):
        o = self.off(addr)
        return self.d[o:self.d.index(b"\0", o)].decode("latin-1")

    def u32(self, addr):
        o = self.off(addr)
        return None if o is None else struct.unpack_from("<I", self.d, o)[0]

    def in_text(self, addr):
        name, va, vsz, ra, rsz, ch = self.text
        return self.base + va <= addr < self.base + va + vsz


def import_fn_name(name):
    special = {"??2@YAPAXI@Z": "op_new", "??3@YAXPAX@Z": "op_delete", "?terminate@@YAXXZ": "terminate"}
    if name in special:
        return special[name]
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


# ------------------------------------------------------------------------------------------------ operands

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

CC = {"o": 0, "no": 1, "b": 2, "nae": 2, "c": 2, "ae": 3, "nb": 3, "nc": 3, "e": 4, "z": 4, "ne": 5, "nz": 5,
      "be": 6, "na": 6, "a": 7, "nbe": 7, "s": 8, "ns": 9, "p": 10, "pe": 10, "np": 11, "po": 11,
      "l": 12, "nge": 12, "ge": 13, "nl": 13, "le": 14, "ng": 14, "g": 15, "nle": 15}


class Unsupported(Exception):
    pass


class Fn:
    """one guest function being translated"""

    def __init__(self, tr, entry):
        self.tr, self.entry = tr, entry
        self.ins = {}              # address -> capstone insn
        self.targets = set()       # addresses that need a label
        self.tables = {}           # address of the table jmp -> list of targets
        self.calls = set()

    def reg_read(self, r):
        if r in REG32:
            return "c->" + r
        if r in REG16:
            return "(c->%s & 0xffffu)" % REG16[r]
        if r in REG8L:
            return "(c->%s & 0xffu)" % REG8L[r]
        if r in REG8H:
            return "((c->%s >> 8) & 0xffu)" % REG8H[r]
        raise Unsupported("register " + r)

    def reg_write(self, r, v):
        if r in REG32:
            return "c->%s = (uint32_t)(%s);" % (r, v)
        if r in REG16:
            g = REG16[r]
            return "c->%s = (c->%s & 0xffff0000u) | ((uint32_t)(%s) & 0xffffu);" % (g, g, v)
        if r in REG8L:
            g = REG8L[r]
            return "c->%s = (c->%s & 0xffffff00u) | ((uint32_t)(%s) & 0xffu);" % (g, g, v)
        if r in REG8H:
            g = REG8H[r]
            return "c->%s = (c->%s & 0xffff00ffu) | (((uint32_t)(%s) & 0xffu) << 8);" % (g, g, v)
        raise Unsupported("register " + r)

    def addr(self, x, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append("c->" + x.reg_name(m.base))
        if m.index:
            idx = "c->" + x.reg_name(m.index)
            parts.append(idx if m.scale == 1 else "%s * %du" % (idx, m.scale))
        if m.disp or not parts:
            parts.append("0x%xu" % (m.disp & 0xffffffff))
        if m.segment and x.reg_name(m.segment) == "fs":
            parts.append("c->fs_base")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read(self, x, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(x.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return "0x%xu" % (op.imm & ((1 << (8 * size)) - 1) if size < 4 else op.imm & 0xffffffff)
        a = self.addr(x, op)
        return {1: "rd8(c, %s)", 2: "rd16(c, %s)", 4: "rd32(c, %s)"}[size] % a

    def write(self, x, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(x.reg_name(op.reg), v)
        a = self.addr(x, op)
        return {1: "wr8(c, %s, (uint8_t)(%s));", 2: "wr16(c, %s, (uint16_t)(%s));",
                4: "wr32(c, %s, (uint32_t)(%s));"}[size] % (a, v)


# ------------------------------------------------------------------------------------------------ translator


class Translator:
    def __init__(self, image, known_entries):
        self.img = image
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.known = set(known_entries)       # function starts (Ghidra's list)
        self.hooks = set()                    # functions that report their entry (x86_on_enter)
        self.replaced = set()                 # functions a hand port provides: emitted as f_XXXXXXXX_recomp
        self.thunks = {}                      # thunk address -> import name
        self.used_imports = set()
        self.fns = {}

    def insn(self, addr):
        o = self.img.off(addr)
        if o is None:
            raise Unsupported("no code at 0x%x" % addr)
        for x in self.md.disasm(self.img.d[o:o + 16], addr):
            return x
        raise Unsupported("cannot decode 0x%x" % addr)

    def thunk(self, addr):
        """if addr is `jmp dword ptr [IAT]`, the import's name"""
        if addr in self.thunks:
            return self.thunks[addr]
        try:
            x = self.insn(addr)
        except Unsupported:
            return None
        name = None
        if x.mnemonic == "jmp" and len(x.operands) == 1 and x.operands[0].type == X.X86_OP_MEM:
            m = x.operands[0].mem
            if not m.base and not m.index and (m.disp & 0xffffffff) in self.img.imports:
                name = self.img.imports[m.disp & 0xffffffff][1]
        self.thunks[addr] = name
        return name

    def discover(self, fn):
        todo = [fn.entry]
        fn.targets.add(fn.entry)
        while todo:
            a = todo.pop()
            while a not in fn.ins:
                x = self.insn(a)
                fn.ins[a] = x
                mn = x.mnemonic
                nxt = a + x.size
                if mn in ("ret", "retn", "int3", "hlt", "ud2"):
                    break
                if mn == "call":
                    op = x.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if not self.thunk(t):
                            fn.calls.add(t)
                    a = nxt
                    continue
                if mn == "jmp":
                    op = x.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if self.thunk(t) or (t in self.known and t != fn.entry):
                            if not self.thunk(t):
                                fn.calls.add(t)
                            break
                        fn.targets.add(t)
                        a = t
                        continue
                    if op.type == X.X86_OP_MEM and op.mem.index and not op.mem.base and op.mem.scale == 4:
                        tab = self.table(fn, x)
                        fn.tables[a] = tab
                        for t in tab:
                            fn.targets.add(t)
                            todo.append(t)
                    break
                if mn.startswith("j") or mn in ("loop", "loope", "loopne", "jecxz"):
                    t = x.operands[0].imm & 0xffffffff
                    fn.targets.add(t)
                    todo.append(t)
                    a = nxt
                    continue
                a = nxt

    def table(self, fn, x):
        """targets of `jmp [idx*4 + table]`: entries while they point into this function's code area"""
        base = x.operands[0].mem.disp & 0xffffffff
        out = []
        # bound from a preceding `cmp idx, N` (then `ja default`): decode the few instructions before
        # the jump from every nearby start that lines up with it, and look for the compare
        bound = None
        idx = x.reg_name(x.operands[0].mem.index)
        a = x.address
        for back in range(2, 24):
            seq = []
            o = self.img.off(a - back)
            if o is None:
                continue
            for y in self.md.disasm(self.img.d[o:o + back], a - back):
                seq.append(y)
            if not seq or seq[-1].address + seq[-1].size != a:
                continue
            for y in reversed(seq[-4:]):
                if y.mnemonic == "cmp" and len(y.operands) == 2 and y.operands[0].type == X.X86_OP_REG                         and y.operands[1].type == X.X86_OP_IMM and y.reg_name(y.operands[0].reg) == idx:
                    bound = y.operands[1].imm + 1
                    break
            if bound is not None:
                break
        n = 0
        while True:
            t = self.img.u32(base + 4 * n)
            if t is None or not self.img.in_text(t) or (bound is not None and n >= bound) or n > 4096:
                break
            if bound is None and n and t in self.known and t != fn.entry:
                break
            out.append(t)
            n += 1
        if not out:
            raise Unsupported("empty jump table at 0x%x" % x.address)
        return out

    # ---------------------------------------------------------------- emission

    def emit_fn(self, fn):
        fn_out = []
        addrs = sorted(fn.ins)
        # fallthrough continuity: an instruction whose next address is not the next emitted one
        need_goto = {}
        for i, a in enumerate(addrs):
            x = fn.ins[a]
            nxt = a + x.size
            if x.mnemonic in ("ret", "retn", "jmp", "int3", "hlt", "ud2"):
                continue
            if i + 1 >= len(addrs) or addrs[i + 1] != nxt:
                need_goto[a] = nxt
                fn.targets.add(nxt)
        name = "f_%08x_recomp" % fn.entry if fn.entry in self.replaced else "f_%08x" % fn.entry
        out = ["static void %s(cpu *c)" % name, "{"]
        if fn.entry in self.hooks:
            out.append("    if (x86_on_enter) x86_on_enter(c, 0x%xu);" % fn.entry)
        for a in addrs:
            if a in fn.targets:
                out.append("L_%08x:;" % a)
            x = fn.ins[a]
            try:
                body = self.emit(fn, x)
            except Unsupported as e:
                body = ['x86_fail(c, 0x%xu, "unsupported: %s");' % (a, str(e).replace('"', "'"))]
            out.append("    /* %08x  %s %s */" % (a, x.mnemonic, x.op_str))
            for line in body:
                out.append("    " + line)
            if a in need_goto:
                nxt = need_goto[a]
                if nxt in fn.ins:
                    out.append("    goto L_%08x;" % nxt)
                else:
                    out.append('    x86_fail(c, 0x%xu, "fell off the translated code");' % nxt)
        out.append('    x86_fail(c, 0x%xu, "end of function");' % fn.entry)
        out.append("}")
        return out

    def import_call(self, name, ret):
        """a call to an imported function. _setjmp3 is special: the host setjmp has to happen in the
        calling function's own frame, so that a later longjmp can come back into it"""
        f = import_fn_name(name)
        self.used_imports.add(f)
        if f == "_setjmp3":
            return ["{ jmp_buf *hb = x86_jmpbuf(c, rd32(c, c->esp));",
                    "  push32(c, 0x%xu);" % ret,
                    "  if (!setjmp(*hb)) imp__setjmp3(c); }"]
        return ["push32(c, 0x%xu);" % ret, "imp_%s(c);" % f]

    def call_to(self, fn, t, ret):
        """statements for a call from `ret - size` to t (return address `ret`)"""
        name = self.thunk(t)
        if name:
            return self.import_call(name, ret)
        return ["push32(c, 0x%xu);" % ret, "f_%08x(c);" % t]

    def emit(self, fn, x):
        mn = x.mnemonic
        ops = x.operands
        a = x.address
        ret = a + x.size
        b = x.bytes
        # prefixes
        if mn.startswith("rep ") or mn.startswith("repne ") or mn.startswith("repe ") or mn.startswith("repz ") \
                or mn.startswith("repnz "):
            pre, mn = mn.split(" ", 1)
            rep = 1
        else:
            pre, rep = "", 0
        # x87 by bytes
        k = 0
        while b[k] in (0x66, 0x67, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x9b):
            k += 1
        if 0xd8 <= b[k] <= 0xdf:
            return self.x87(fn, x, b[k], b[k + 1])
        if mn in ("wait", "fwait", "nop", "cld", "fnop"):
            return [";"]
        if mn == "std":
            raise Unsupported("std")

        def sz(o):
            return o.size

        if mn == "mov":
            return [fn.write(x, ops[0], fn.read(x, ops[1], sz(ops[0])))]
        if mn in ("movzx", "movsx"):
            v = fn.read(x, ops[1])
            if mn == "movsx":
                v = "(uint32_t)(int32_t)(%s)(%s)" % ("int8_t" if ops[1].size == 1 else "int16_t", v)
            return [fn.write(x, ops[0], v)]
        if mn == "lea":
            return [fn.write(x, ops[0], fn.addr(x, ops[1]))]
        if mn == "xchg":
            s = sz(ops[0])
            return ["{ uint32_t t0 = %s, t1 = %s;" % (fn.read(x, ops[0]), fn.read(x, ops[1])),
                    "  " + fn.write(x, ops[0], "t1"), "  " + fn.write(x, ops[1], "t0") + " }"]
        if mn in ("add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test"):
            s = sz(ops[0])
            av, bv = fn.read(x, ops[0]), fn.read(x, ops[1], s)
            if mn in ("add", "adc", "sub", "sbb"):
                e = "op_%s(c, %d, %s, %s)" % (mn, s, av, bv)
            elif mn == "cmp":
                return ["op_sub(c, %d, %s, %s);" % (s, av, bv)]
            elif mn == "test":
                return ["op_logic(c, %d, (%s) & (%s));" % (s, av, bv)]
            else:
                e = "op_logic(c, %d, (%s) %s (%s))" % (s, av, {"and": "&", "or": "|", "xor": "^"}[mn], bv)
            return ["{ uint32_t r = %s; %s }" % (e, fn.write(x, ops[0], "r"))]
        if mn in ("inc", "dec", "neg"):
            s = sz(ops[0])
            return ["{ uint32_t r = op_%s(c, %d, %s); %s }" % (mn, s, fn.read(x, ops[0]), fn.write(x, ops[0], "r"))]
        if mn == "not":
            return [fn.write(x, ops[0], "~(%s)" % fn.read(x, ops[0]))]
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror"):
            kind = {"shl": 0, "sal": 0, "shr": 1, "sar": 2, "rol": 3, "ror": 4}[mn]
            cnt = fn.read(x, ops[1]) if len(ops) > 1 else "1u"
            return ["{ uint32_t r = op_shift(c, %d, %d, %s, %s); %s }" % (
                kind, sz(ops[0]), fn.read(x, ops[0]), cnt, fn.write(x, ops[0], "r"))]
        if mn in ("shld", "shrd"):
            return ["{ uint32_t r = op_shd(c, %d, %s, %s, %s); %s }" % (
                mn == "shld", fn.read(x, ops[0]), fn.read(x, ops[1]), fn.read(x, ops[2]), fn.write(x, ops[0], "r"))]
        if mn == "imul":
            if len(ops) == 1:
                s = sz(ops[0])
                if s != 4:
                    raise Unsupported("imul r/m%d" % (8 * s))
                return ["{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t)%s;" % fn.read(x, ops[0]),
                        "  c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32);",
                        "  fl_set(c, 4, c->eax, p != (int32_t)p, p != (int32_t)p); }"]
            if len(ops) == 2:
                src1, src2 = fn.read(x, ops[0]), fn.read(x, ops[1], sz(ops[0]))
            else:
                src1, src2 = fn.read(x, ops[1]), fn.read(x, ops[2], sz(ops[0]))
            s = sz(ops[0])
            if s == 2:
                return ["{ int32_t p = (int32_t)(int16_t)(%s) * (int16_t)(%s);" % (src1, src2),
                        "  fl_set(c, 2, (uint32_t)p, p != (int16_t)p, p != (int16_t)p); %s }" % fn.write(x, ops[0], "p")]
            return ["{ int64_t p = (int64_t)(int32_t)(%s) * (int32_t)(%s);" % (src1, src2),
                    "  fl_set(c, 4, (uint32_t)p, p != (int32_t)p, p != (int32_t)p); %s }" % fn.write(x, ops[0], "p")]
        if mn == "mul":
            if sz(ops[0]) != 4:
                raise Unsupported("mul r/m%d" % (8 * sz(ops[0])))
            return ["{ uint64_t p = (uint64_t)c->eax * %s;" % fn.read(x, ops[0]),
                    "  c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32);",
                    "  fl_set(c, 4, c->eax, c->edx != 0, c->edx != 0); }"]
        if mn in ("div", "idiv"):
            if sz(ops[0]) != 4:
                raise Unsupported("%s r/m%d" % (mn, 8 * sz(ops[0])))
            d = fn.read(x, ops[0])
            if mn == "div":
                return ["{ uint64_t n = ((uint64_t)c->edx << 32) | c->eax; uint32_t d = %s;" % d,
                        '  if (!d || n / d > 0xffffffffull) x86_fail(c, 0x%xu, "divide error");' % a,
                        "  c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }"]
            return ["{ int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); int32_t d = (int32_t)%s;" % d,
                    '  if (!d || (n / d) != (int32_t)(n / d)) x86_fail(c, 0x%xu, "divide error");' % a,
                    "  c->eax = (uint32_t)(int32_t)(n / d); c->edx = (uint32_t)(int32_t)(n % d); }"]
        if mn == "cdq":
            return ["c->edx = (c->eax & 0x80000000u) ? 0xffffffffu : 0;"]
        if mn == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"]
        if mn == "cbw":
            return ["c->eax = (c->eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)c->eax & 0xffffu);"]
        if mn == "cwd":
            return ["c->edx = (c->edx & 0xffff0000u) | ((c->eax & 0x8000u) ? 0xffffu : 0);"]
        if mn == "push":
            if ops[0].type == X.X86_OP_IMM:
                return ["push32(c, 0x%xu);" % (ops[0].imm & 0xffffffff)]
            if ops[0].size != 4:
                raise Unsupported("push of %d bytes" % ops[0].size)
            return ["{ uint32_t v = %s; push32(c, v); }" % fn.read(x, ops[0])]
        if mn == "pop":
            return ["{ uint32_t v = pop32(c); %s }" % fn.write(x, ops[0], "v")]
        if mn == "leave":
            return ["c->esp = c->ebp;", "c->ebp = pop32(c);"]
        if mn == "lahf":
            return ["c->eax = (c->eax & 0xffff00ffu) | (x86_lahf(c) << 8);"]
        if mn == "sahf":
            return ["x86_sahf(c);"]
        if mn == "bt":
            s = sz(ops[0])
            return ["c->fl_cf = (uint8_t)((%s >> ((%s) & %d)) & 1);" % (fn.read(x, ops[0]), fn.read(x, ops[1]), 8 * s - 1)]
        if mn == "bswap":
            r = x.reg_name(ops[0].reg)
            return ["{ uint32_t v = c->%s; c->%s = (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); }" % (r, r)]
        if mn.startswith("set"):
            return [fn.write(x, ops[0], "x86_cond(c, %d)" % CC[mn[3:]])]
        if mn.startswith("cmov"):
            return ["if (x86_cond(c, %d)) %s" % (CC[mn[4:]], fn.write(x, ops[0], fn.read(x, ops[1])))]
        if mn in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "scasb", "scasw", "scasd",
                  "cmpsb", "cmpsw", "cmpsd"):
            s = {"b": 1, "w": 2, "d": 4}[mn[-1]]
            f = {"movs": "x86_rep_movs", "stos": "x86_rep_stos", "scas": "x86_repne_scas",
                 "cmps": "x86_repe_cmps"}[mn[:4]]
            if mn[:4] == "scas" and pre and pre not in ("repne", "repnz"):
                raise Unsupported("repe scas")
            if mn[:4] == "cmps" and pre and pre not in ("repe", "repz"):
                raise Unsupported("repne cmps")
            return ["%s(c, %d, %d);" % (f, s, rep)]
        if mn == "call":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return self.call_to(fn, op.imm & 0xffffffff, ret)
            if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index \
                    and (op.mem.disp & 0xffffffff) in self.img.imports:
                return self.import_call(self.img.imports[op.mem.disp & 0xffffffff][1], ret)
            return ["{ uint32_t t = %s; push32(c, 0x%xu); x86_icall(c, t); }" % (fn.read(x, op), ret)]
        if mn in ("ret", "retn"):
            n = ops[0].imm if ops else 0
            return ["c->esp += %du;" % (4 + n), "return;"]
        if mn == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                name = self.thunk(t)
                if name:
                    f = import_fn_name(name)
                    self.used_imports.add(f)
                    return ["imp_%s(c);" % f, "return;"]
                if t in fn.ins and not (t in self.known and t != fn.entry):
                    return ["goto L_%08x;" % t]
                return ["f_%08x(c);" % t, "return;"]
            if a in fn.tables:
                idx = fn.read(x, op).split("(c, ")[1]
                m = op.mem
                ireg = "c->" + x.reg_name(m.index)
                lines = ["switch (%s) {" % ireg]
                for i, t in enumerate(fn.tables[a]):
                    lines.append("case %du: goto L_%08x;" % (i, t))
                lines.append('default: x86_fail(c, 0x%xu, "jump table index"); }' % a)
                return lines
            if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index \
                    and (op.mem.disp & 0xffffffff) in self.img.imports:
                f = import_fn_name(self.img.imports[op.mem.disp & 0xffffffff][1])
                self.used_imports.add(f)
                return ["imp_%s(c);" % f, "return;"]
            return ["{ uint32_t t = %s; x86_icall(c, t); } return;" % fn.read(x, op)]
        if mn == "jecxz":
            return ["if (!c->ecx) goto L_%08x;" % (ops[0].imm & 0xffffffff)]
        if mn == "loop":
            return ["if (--c->ecx) goto L_%08x;" % (ops[0].imm & 0xffffffff)]
        if mn.startswith("j"):
            return ["if (x86_cond(c, %d)) goto L_%08x;" % (CC[mn[1:]], ops[0].imm & 0xffffffff)]
        if mn in ("int3", "hlt", "ud2"):
            return ['x86_fail(c, 0x%xu, "%s");' % (a, mn)]
        raise Unsupported(mn)

    # ---------------------------------------------------------------- x87, from the bytes

    def x87(self, fn, x, op, modrm):
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        mem = [o for o in x.operands if o.type == X.X86_OP_MEM]
        A = fn.addr(x, mem[0]) if mem and mod != 3 else None
        E = "FENV(c)"
        arith = ["fx_add", "fx_mul", None, None, "fx_sub", "fx_subr", "fx_div", "fx_divr"]

        def binop(kind, dst, a_, b_):
            if kind == "fx_subr":
                return "%s = fx_sub(%s, %s, %s);" % (dst, b_, a_, E)
            if kind == "fx_divr":
                return "%s = fx_div(%s, %s, %s);" % (dst, b_, a_, E)
            return "%s = %s(%s, %s, %s);" % (dst, kind, a_, b_, E)

        if mod != 3:
            if op in (0xd8, 0xdc, 0xda, 0xde):       # ST0 op memory (float, double, int32, int16)
                src = {0xd8: "fx_from_f32(rd32(c, %s))", 0xdc: "fx_from_f64(rd64(c, %s))",
                       0xda: "fx_from_i64((int32_t)rd32(c, %s))", 0xde: "fx_from_i64((int16_t)rd16(c, %s))"}[op] % A
                if reg in (2, 3):
                    out = ["fcom_set(c, ST(0), %s);" % src]
                    if reg == 3:
                        out.append("fpop(c);")
                    return out
                return [binop(arith[reg], "ST(0)", "ST(0)", src)]
            if op == 0xd9:
                if reg == 0:
                    return ["{ fx80 v = fx_from_f32(rd32(c, %s)); fpush(c, v); }" % A]
                if reg in (2, 3):
                    out = ["wr32(c, %s, fx_to_f32(ST(0), %s));" % (A, E)]
                    if reg == 3:
                        out.append("fpop(c);")
                    return out
                if reg == 5:
                    return ["c->fcw = rd16(c, %s);" % A]
                if reg == 7:
                    return ["wr16(c, %s, c->fcw);" % A]
                raise Unsupported("d9 /%d mem" % reg)
            if op == 0xdb:
                if reg == 0:
                    return ["{ fx80 v = fx_from_i64((int32_t)rd32(c, %s)); fpush(c, v); }" % A]
                if reg in (2, 3):
                    out = ["wr32(c, %s, (uint32_t)fx_to_int(ST(0), %s, 32));" % (A, E)]
                    if reg == 3:
                        out.append("fpop(c);")
                    return out
                if reg == 5:
                    return ["{ uint8_t t[10]; x86_read(c, %s, t, 10); fx80 v = fx_from_tbyte(t); fpush(c, v); }" % A]
                if reg == 7:
                    return ["{ uint8_t t[10]; fx_to_tbyte(ST(0), t); x86_write(c, %s, t, 10); fpop(c); }" % A]
                raise Unsupported("db /%d mem" % reg)
            if op == 0xdd:
                if reg == 0:
                    return ["{ fx80 v = fx_from_f64(rd64(c, %s)); fpush(c, v); }" % A]
                if reg in (2, 3):
                    out = ["wr64(c, %s, fx_to_f64(ST(0), %s));" % (A, E)]
                    if reg == 3:
                        out.append("fpop(c);")
                    return out
                if reg == 7:
                    return ["wr16(c, %s, fsw(c));" % A]
                raise Unsupported("dd /%d mem" % reg)
            if op == 0xdf:
                if reg == 0:
                    return ["{ fx80 v = fx_from_i64((int16_t)rd16(c, %s)); fpush(c, v); }" % A]
                if reg in (2, 3):
                    out = ["wr16(c, %s, (uint16_t)fx_to_int(ST(0), %s, 16));" % (A, E)]
                    if reg == 3:
                        out.append("fpop(c);")
                    return out
                if reg == 5:
                    return ["{ fx80 v = fx_from_i64((int64_t)rd64(c, %s)); fpush(c, v); }" % A]
                if reg == 7:
                    return ["wr64(c, %s, (uint64_t)fx_to_int(ST(0), %s, 64)); fpop(c);" % (A, E)]
                raise Unsupported("df /%d mem" % reg)
            raise Unsupported("x87 %02x /%d mem" % (op, reg))
        i = rm
        if op == 0xd8:          # ST0 = ST0 op STi
            if reg in (2, 3):
                out = ["fcom_set(c, ST(0), ST(%d));" % i]
                if reg == 3:
                    out.append("fpop(c);")
                return out
            return [binop(arith[reg], "ST(0)", "ST(0)", "ST(%d)" % i)]
        if op in (0xdc, 0xde):  # STi = STi op ST0 (the E0/E8, F0/F8 pairs are swapped against D8)
            k = {0: "fx_add", 1: "fx_mul", 4: "fx_subr", 5: "fx_sub", 6: "fx_divr", 7: "fx_div"}.get(reg)
            if op == 0xde and modrm == 0xd9:
                return ["fcom_set(c, ST(0), ST(1));", "fpop(c);", "fpop(c);"]
            if k is None:
                raise Unsupported("x87 %02x %02x" % (op, modrm))
            out = [binop(k, "ST(%d)" % i, "ST(%d)" % i, "ST(0)")]
            if op == 0xde:
                out.append("fpop(c);")
            return out
        if op == 0xd9:
            if reg == 0:
                return ["{ fx80 v = ST(%d); fpush(c, v); }" % i]
            if reg == 1:
                return ["{ fx80 t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (i, i)]
            simple = {
                0xd0: [";"], 0xe0: ["ST(0) = fx_neg(ST(0));"], 0xe1: ["ST(0) = fx_abs(ST(0));"],
                0xe4: ["fcom_set(c, ST(0), fx_zero(0));"],
                0xe8: ["fpush(c, fx_const(0));"], 0xe9: ["fpush(c, fx_const(1));"], 0xea: ["fpush(c, fx_const(2));"],
                0xeb: ["fpush(c, fx_const(3));"], 0xec: ["fpush(c, fx_const(4));"], 0xed: ["fpush(c, fx_const(5));"],
                0xee: ["fpush(c, fx_zero(0));"],
                0xf0: ["ST(0) = fx_f2xm1(ST(0));"],
                0xf1: ["ST(1) = fx_yl2x(ST(1), ST(0));", "fpop(c);"],
                0xf6: ["c->ftop = (c->ftop - 1) & 7;"], 0xf7: ["c->ftop = (c->ftop + 1) & 7;"],
                0xfa: ["ST(0) = fx_sqrt(ST(0), %s);" % E],
                0xfc: ["ST(0) = fx_rndint(ST(0), %s);" % E],
                0xfd: ["ST(0) = fx_scale(ST(0), ST(1));"],
                0xfe: ["ST(0) = fx_sin(ST(0)); c->c2 = 0;"],
                0xff: ["ST(0) = fx_cos(ST(0)); c->c2 = 0;"],
            }
            if modrm in simple:
                return simple[modrm]
            raise Unsupported("x87 d9 %02x" % modrm)
        if op == 0xda and modrm == 0xe9:
            return ["fcom_set(c, ST(0), ST(1));", "fpop(c);", "fpop(c);"]
        if op == 0xdb:
            if modrm in (0xe2, 0xe3):
                return [";"] if modrm == 0xe2 else ["c->ftop = 0; c->fcw = 0x037f;"]
            if reg in (5, 6):
                return ["fcomi_set(c, ST(0), ST(%d));" % i]
            raise Unsupported("x87 db %02x" % modrm)
        if op == 0xdd:
            if reg == 0:
                return [";"]                  # ffree
            if reg in (2, 3):
                out = ["ST(%d) = ST(0);" % i]
                if reg == 3:
                    out.append("fpop(c);")
                return out
            if reg in (4, 5):
                out = ["fcom_set(c, ST(0), ST(%d));" % i]
                if reg == 5:
                    out.append("fpop(c);")
                return out
            raise Unsupported("x87 dd %02x" % modrm)
        if op == 0xdf:
            if modrm == 0xe0:
                return ["c->eax = (c->eax & 0xffff0000u) | fsw(c);"]
            if reg in (5, 6):
                return ["fcomi_set(c, ST(0), ST(%d));" % i, "fpop(c);"]
            raise Unsupported("x87 df %02x" % modrm)
        raise Unsupported("x87 %02x %02x" % (op, modrm))

    # ---------------------------------------------------------------- driver

    def run(self, entries, tree):
        todo = list(entries)
        while todo:
            e = todo.pop()
            if e in self.fns:
                continue
            fn = Fn(self, e)
            try:
                self.discover(fn)
            except Unsupported as ex:
                print("x2c: cannot follow 0x%x: %s" % (e, ex), file=sys.stderr)
                fn.ins = {}
            self.fns[e] = fn
            if tree:
                todo.extend(t for t in fn.calls if t not in self.fns)

    @staticmethod
    def write_if_changed(path, text):
        if os.path.exists(path) and open(path, newline="\n").read() == text:
            return False
        open(path, "w", newline="\n").write(text)
        return True

    DATA_IMPORTS = {"_pctype", "__mb_cur_max", "_iob", "_adjust_fdiv", "?openprot@filebuf@@2HB"}

    def output(self, path, split=1):
        """path.c (split == 1) or path_00.c .. and path.h; plus the function and import tables"""
        head = ["/* generated by tools/x2c.py: %d functions recompiled from the engine's machine code. */" % len(self.fns),
                '#include "x86rt.h"', "#include <setjmp.h>", "",
                "#if defined(_MSC_VER)", "#pragma warning(disable: 4102 4146 4244)", "#endif", ""]
        decls = []
        for e in sorted(self.fns):
            decls.append("void f_%08x(cpu *c);" % e)
        called = set()
        for fn in self.fns.values():
            called |= fn.calls
        extra = sorted(t for t in called if t not in self.fns)
        for t in extra:
            decls.append("void f_%08x(cpu *c);" % t)
        # every function import is declared (the runtime implements or refuses each); data imports are
        # filled in by the loader
        fimps = sorted({import_fn_name(n) for (dll, n) in self.img.imports.values() if n not in self.DATA_IMPORTS})
        for f in fimps:
            decls.append("void imp_%s(cpu *c);" % f)
        decls.append("jmp_buf *x86_jmpbuf(cpu *c, uint32_t guest_buf);")
        bodies = []
        for e in sorted(self.fns):
            fn = self.fns[e]
            if not fn.ins:
                bodies.append(['void f_%08x(cpu *c) { x86_fail(c, 0x%xu, "not recompiled"); }' % (e, e)])
                continue
            b = self.emit_fn(fn)
            b[0] = b[0].replace("static void", "void")
            bodies.append(b + [""])
        stubs = ['void f_%08x(cpu *c) { x86_icall(c, 0x%xu); }' % (t, t) for t in extra]
        tables = ["const fn_entry x86_functions[] = {"]
        for e in sorted(self.fns):
            tables.append("    { 0x%08xu, f_%08x }," % (e, e))
        tables.append("};")
        tables.append("const unsigned x86_nfunctions = %d;" % len(self.fns))
        tables.append("const import_entry x86_imports[] = {")
        for slot in sorted(self.img.imports):
            dll, n = self.img.imports[slot]
            fnp = "NULL" if n in self.DATA_IMPORTS else "imp_%s" % import_fn_name(n)
            tables.append('    { 0x%08xu, "%s", "%s", %s },' % (slot, dll, n, fnp))
        tables.append("};")
        tables.append("const unsigned x86_nimports = %d;" % len(self.img.imports))
        base = path[:-2] if path.endswith(".c") else path
        if split <= 1:
            open(base + ".c", "w", newline="\n").write(
                "\n".join(head + decls + [""] + stubs + [""] + [l for b in bodies for l in b] + tables) + "\n")
        else:
            hname = os.path.basename(base) + ".h"
            self.write_if_changed(base + ".h", "\n".join(head + decls) + "\n")
            per = (len(bodies) + split - 1) // split
            for k in range(split):
                part = bodies[k * per:(k + 1) * per]
                lines = ['#include "%s"' % hname, ""] + [l for b in part for l in b]
                if k == 0:
                    lines = ['#include "%s"' % hname, ""] + stubs + [""] + tables + [""] + [l for b in part for l in b]
                if self.write_if_changed("%s_%02d.c" % (base, k), "\n".join(lines) + "\n"):
                    print("x2c: rewrote %s_%02d.c" % (os.path.basename(base), k))
        print("x2c: %d functions, %d imports used (%s)" % (len(self.fns), len(self.used_imports),
                                                          ", ".join(sorted(self.used_imports))))


def ghidra_entries(image_path):
    """function starts from Ghidra's export of this image (decomp/<file name>/_all.c)"""
    ents = set()
    p = os.path.join(ROOT, "decomp", os.path.basename(image_path), "_all.c")
    if os.path.exists(p):
        for m in re.finditer(r"(?m)^// \S+ @ ([0-9a-f]{8})$", open(p, encoding="utf-8", errors="replace").read()):
            ents.add(int(m.group(1), 16))
    else:
        print("x2c: no Ghidra function list at %s" % p, file=sys.stderr)
    return ents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("entries", nargs="+")
    ap.add_argument("--tree", action="store_true")
    ap.add_argument("--image", default=os.path.join(ROOT, "pkg", "ENU.SYN"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "src", "gen_x86.c"))
    ap.add_argument("--split", type=int, default=1, help="write the functions into this many .c files")
    ap.add_argument("--hook", nargs="*", default=[], help="functions that call x86_on_enter(c, addr) first")
    ap.add_argument("--replace", nargs="*", default=[],
                    help="functions ported by hand: the recompiled one becomes f_XXXXXXXX_recomp, the hand port "
                         "defines f_XXXXXXXX")
    ap.add_argument("--replace-list", default=None,
                    help="a file of PORTED(XXXXXXXX, flags) lines (src/ported.h): added to --replace")
    a = ap.parse_args()
    if a.replace_list:
        a.replace += re.findall(r"(?m)^PORTED\(([0-9a-fA-F]{8})", open(a.replace_list).read())
    img = Image(a.image)
    tr = Translator(img, ghidra_entries(a.image))
    tr.hooks = {int(h, 16) for h in a.hook}
    tr.replaced = {int(h, 16) for h in a.replace}
    tr.run([int(e, 16) for e in a.entries], a.tree)
    tr.output(a.out, a.split)


if __name__ == "__main__":
    main()
