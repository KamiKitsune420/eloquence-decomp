r"""delta_lift - lift Eloquence's compiled Delta rule functions (x86 machine code in ENU.SYN) into readable C
that calls the hand-written rule runtime (src/rules*.c) and other rules by name.

  python tools/delta_lift.py [--only 1002c49f,...] [--max N] [-o src/gen] [--report]

Writes (git-ignored, derived from ENU.SYN):
  src/gen/rules_lifted_NN.c   the lifted rules, one C function per rule (LIFTED_FN(address))
  src/gen/lifted.h            PORTED(address, 0) for each: the list x2c --replace-list and difftest take

A rule listed in src/rules/hand.h (HAND_RULE(address)) has been written by hand (src/rules/*.c): it stays in
lifted.h - so x2c keeps replacing it and difftest checks it as a rule - but its lifted body is not emitted.
  src/gen/lift_rt.h           the rule runtime's entry points by name (r_enter, r_var_init, ...)
and prints how many rules were lifted and why the others were refused.

How it works. A rule function (999 of them: they call _setjmp3 and rl_enter) is decoded with x2c's decoder
(tools/x2c.py) and cut into basic blocks. Each block is executed symbolically:
  * esp is tracked as a constant offset from the entry esp; ebp must be the frame pointer (push ebp; mov
    ebp, esp) and locals are addresses fp - k in guest memory (the runtime keeps pointers to them);
  * registers hold symbolic values: constants, frame addresses, parameters (read once at entry: checked
    that nothing writes or takes the address of the parameter slots), C expressions over those, lazy loads
    of guest memory (evaluated where the original loads, or later when nothing that could change the
    memory happens in between), the result of the last call;
  * pushes are collected and become the arguments of the next call, written where the original writes
    them (the runtime functions use their argument slots and their stack frame at the original's esp);
  * the flags are a record of the last flag-setting instruction, turned into a C condition at the jump;
  * at control flow joins, registers whose values differ become C variables assigned on the incoming
    edges; blocks with one predecessor inherit everything.
Anything outside this model makes the tool refuse the function (it stays recompiled): x87 instructions,
indirect calls, callees that are not plain cdecl, reads of undefined registers, flags that cannot be
expressed, stack pointer mismatches at joins, writes to ebp, ... - nothing is guessed. Jump tables (the
backtracking dispatch of large rules) are read from the image, as x2c does, and become switches.

The machine boundary is kept: a lifted rule is entered by a guest call and leaves eax and esp as the
original (src/lift.h). The callee-saved registers are never written (their values are C variables), so they
hold the caller's values throughout, which is what the original's epilogue restores. The _setjmp3 of the
rule becomes a host setjmp in the lifted function's frame keyed to the same guest jmp_buf (as x2c does);
after a longjmp the lifted code goes on with only constants, the frame and the parameters (checked).
"""
import argparse
import collections
import os
import re
import sys

from capstone import x86 as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x2c  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M32 = 0xffffffff

# ------------------------------------------------------------------------------------------------ the runtime

# the rule runtime's entry points the rules call: address -> (name, number of arguments). The names are
# those of src/rules*.c without the rl_ prefix (the adapters are in rules_guest.c); the generated wrappers
# are r_<name>(S(off, ret), args...).
RUNTIME = {
    0x101315e0: ("enter", 6), 0x10131790: ("leave", 1), 0x101311a0: ("backtrack", 2),
    0x10130ea0: ("var_init", 4), 0x101310b0: ("var_init_sync", 2),
    0x10131cd0: ("push_val", 2), 0x10131d10: ("push_sym", 2), 0x10131d40: ("push_int", 2),
    0x10131d70: ("push_short", 2), 0x10131da0: ("push_field", 3), 0x10131e90: ("pop_into", 2),
    0x10131f00: ("cmp_popped_byte", 1), 0x10131f90: ("cut", 1), 0x10131ff0: ("push_retry", 2),
    0x10132040: ("push_retry_pos", 2), 0x101320e0: ("push_down", 1), 0x10132120: ("push_up", 1),
    0x10132160: ("aux_1013a040", 1),
    0x10132170: ("cmp_short_ne", 3), 0x101321d0: ("cmp_short_lt", 3), 0x10132230: ("cmp_short_le", 3),
    0x10132290: ("cmp_short_ge", 3), 0x101322f0: ("cmp_short_eq", 3), 0x10132350: ("compare_ne", 3),
    0x10132370: ("cmp_ne", 1), 0x101323c0: ("cmp_eq", 1), 0x10132410: ("cmp_le", 1), 0x10132460: ("cmp_lt", 1),
    0x101324b0: ("cmp_ge", 1), 0x10132500: ("cmp_gt", 1),
    0x10132550: ("field_ne", 4), 0x101325e0: ("set_short", 3), 0x101326c0: ("push_down_retry", 2),
    0x101326e0: ("push_down_retry_pos", 2), 0x10132700: ("set_ab", 3), 0x10132a20: ("insert_list", 5),
    0x10132a70: ("insert_list_shorts", 5), 0x10132ac0: ("succeed", 2), 0x10132b80: ("cmp_result_ne", 1),
    0x10132ba0: ("cmp_result_eq", 1), 0x10132bc0: ("ab_test", 1), 0x10132c40: ("match_string", 4),
    0x10132de0: ("match_shorts", 4), 0x10132f20: ("advance_to_a", 1), 0x10132f80: ("align", 4),
    0x101330b0: ("val_call", 3), 0x10133110: ("lookup", 2), 0x10133140: ("lookup_b", 2),
    0x10133170: ("ab_call", 5), 0x101331c0: ("emit", 3), 0x101331e0: ("emit_val", 3), 0x10133210: ("set_token", 2),
    0x10133250: ("assign", 3), 0x101332d0: ("touch", 2), 0x10133300: ("mark_here", 3),
    0x101333c0: ("take_token", 2), 0x10133480: ("edit_count", 4), 0x10133550: ("goto_a", 2),
    0x101335e0: ("goto_a_back", 2), 0x101337b0: ("goto_a_moved", 2), 0x10133840: ("goto_a_back_moved", 2),
    0x10133670: ("goto_val", 3), 0x10133710: ("goto_val_back", 3), 0x101338d0: ("commit", 2),
    0x10133960: ("start_back_step", 6), 0x10133a50: ("start_back_token2", 6),
    0x10133bb0: ("start_back_token", 6), 0x10133ce0: ("start_step", 7), 0x10133fc0: ("start_step_back", 7),
    0x10133e10: ("start_span", 7), 0x101340f0: ("start_span_back", 7), 0x10134280: ("compare_eq_next", 3),
    0x10134300: ("assign_then", 5), 0x10134400: ("for_step", 6), 0x10134500: ("for_done", 4),
    0x101345e0: ("set_labels", 3), 0x10134690: ("set_a", 2), 0x101346b0: ("set_b", 2),
    0x101346d0: ("set_a_offset", 3), 0x10134720: ("sv_bound_a1", 2), 0x10134780: ("sv_bound_b1", 2),
    0x101347e0: ("sv_bound_a0", 2), 0x10134840: ("sv_bound_b0", 2), 0x101348a0: ("sv_move_a", 2),
    0x101348e0: ("sv_move_b", 2), 0x10134920: ("sv_move_a_eng", 2), 0x10134960: ("a_bound_01", 2),
    0x101349c0: ("a_bound_10", 2), 0x10134a20: ("a_move", 2), 0x10134a60: ("a_move_eng", 2),
    0x10134aa0: ("get_a", 2), 0x10134b00: ("get_b", 2), 0x10134b60: ("align1", 3), 0x10134c80: ("no_step", 1),
    0x10134ca0: ("no_next_token", 1), 0x10134cc0: ("reach", 2), 0x10134d90: ("mark_a_on", 2),
    0x10134db0: ("mark_a_off", 2), 0x10134dd0: ("edit_bytes", 4), 0x10134e40: ("edit_a", 2),
    0x10134e80: ("edit_b_a", 2), 0x10134ec0: ("edit_a_b", 2), 0x10134f00: ("reset_streams", 3),
    0x10134fd0: ("edit_a_token", 2), 0x10135010: ("delete_between", 3), 0x10135060: ("set_field_val", 5),
    0x10135150: ("set_field_sym", 5), 0x101351d0: ("set_field_short", 5), 0x10135250: ("insert_value", 4),
    0x101353d0: ("insert_text", 5), 0x10135420: ("insert_text_a", 5), 0x10135470: ("insert_value_b", 4),
    0x10135660: ("measure", 3), 0x10135730: ("match_pattern", 4), 0x10135820: ("match_table", 3),
    0x10135890: ("set_result_count", 1), 0x101358b0: ("mod", 4),
}
RT_BY_NAME = {v[0]: k for k, v in RUNTIME.items()}
SETJMP_THUNK_NAME = "_setjmp3"
# msvcrt functions the rules call that are plain cdecl (arguments on the stack, result in eax)
IMPORTS_CDECL = {"memset", "memcpy", "memmove", "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strcat",
                 "sprintf", "atoi", "atol", "strtol", "malloc", "free", "calloc", "realloc", "isspace", "toupper",
                 "tolower", "strchr", "strstr", "_strdup", "_stricmp", "_strnicmp",
                 "_ftol",   # _ftol: st0 to eax:edx on the machine's x87, no stack arguments
                 "exp", "log", "pow"}  # doubles on the stack (stored there by the x87), result in st0

STREAMS = ["char_count", "inp", "phone", "morph", "word", "inton_phr", "klatt", "syllable", "Ms"]
TYPE_NAMES = {0xffffffff: "T_SYM8", 0xfffffffe: "T_SYM16", 0xfffffffd: "T_INT", 0xfffffffc: "T_SHORT",
              0xfffffffb: "T_DOUBLE", 0xfffffffa: "T_SYNC"}
# the bytes a variable of each kind takes in the frame (short type + data)
VALUE_SIZE = {"sym": 4, "int": 6, "short": 4, "dbl": 10, "sync": 6}
TYPE_VAR = {0xffffffff: "sym", 0xfffffffe: "sym", 0xfffffffd: "int", 0xfffffffc: "short", 0xfffffffb: "dbl",
            0xfffffffa: "sync"}


class Refuse(Exception):
    pass


# ------------------------------------------------------------------------------------------------ values

def cst(n):
    n &= M32
    return "%du" % n if n < 10 else "0x%xu" % n


class V:
    """a symbolic 32-bit value. kind: 'k' constant n, 'fp' the address fp + n, 'p' parameter n, 'in' the
    entry value of callee-saved register n, 'x' a C expression, 'u' undefined. c is the C expression (with
    @..@ markers for names given at the end); reads: what it reads ('f', off, size) frame memory at fp +
    off, ('m',) other memory, ('r',) the machine's registers; hv: host variables it uses; bits: an upper
    bound of its width; atom: needs no parentheses"""
    __slots__ = ("kind", "n", "c", "reads", "hv", "bits", "atom", "base", "db")

    def __init__(self, kind, c, n=0, reads=frozenset(), hv=frozenset(), bits=32, atom=False, base=None, db=32):
        self.kind, self.c, self.n, self.reads, self.hv, self.bits, self.atom, self.base, self.db = \
            kind, c, n, frozenset(reads), frozenset(hv), bits, atom, base, db

    def key(self):
        return (self.kind, self.c, self.n, self.db)

    def __eq__(self, o):
        return isinstance(o, V) and self.key() == o.key()

    def __hash__(self):
        return hash(self.key())

    def __repr__(self):
        return "V(%s,%s)" % (self.kind, self.c)

    @property
    def lazy(self):
        return bool(self.reads)

    @property
    def pure(self):
        return self.kind in ("k", "fp", "p", "in") or (self.kind == "x" and not self.reads and not self.hv)

    def paren(self):
        return self.c if self.atom else "(" + self.c + ")"


UNDEF = V("u", "@UNDEF@", atom=True)
ALL_REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp")


def K(n):
    n &= M32
    return V("k", cst(n), n, bits=max(1, n.bit_length()), atom=True)


def FP(off):
    return V("fp", "@F%d@" % off, off, atom=True)


def X_(c, *parts, reads=(), hv=(), bits=32, atom=False, base=None, db=None):
    """an expression over parts; db (its defined low bits) is what all parts define: the low bits of a sum,
    difference, product or logical combination depend only on the low bits of the operands"""
    r, h = set(reads), set(hv)
    d = 32
    for p in parts:
        r |= p.reads
        h |= p.hv
        d = min(d, p.db)
    return V("x", c, reads=r, hv=h, bits=bits, atom=atom, base=base, db=d if db is None else db)


def is_undef(v):
    return v.kind == "u" or "@UNDEF@" in v.c


def v_add(a, b):
    if a.kind == "k" and b.kind == "k":
        return K(a.n + b.n)
    if b.kind == "k" and b.n == 0:
        return a
    if a.kind == "k" and a.n == 0:
        return b
    if a.kind == "k":
        a, b = b, a
    if b.kind == "k":
        if a.kind == "fp":
            d = b.n if b.n < 0x80000000 else b.n - 0x100000000
            return FP(a.n + d)
        base, off = (a.base if a.base is not None else a), 0
        if a.base is not None:
            off = a.n
        off = (off + b.n) & M32
        if off == 0:
            return base
        s = "%s + %s" % (base.c if base.atom or base.kind != "x" else base.paren(), cst(off)) if off < 0x80000000 \
            else "%s - %s" % (base.paren(), cst(-off))
        v = X_(s, base, base=base)
        v.n = off
        return v
    return X_("%s + %s" % (a.paren(), b.paren()), a, b)


def v_sub(a, b):
    if b.kind == "k":
        return v_add(a, K(-b.n))
    if a.kind == "fp" and b.kind == "fp":
        return K(a.n - b.n)
    if a == b and not a.lazy:
        return K(0)
    return X_("%s - %s" % (a.paren(), b.paren()), a, b)


def v_logic(op, a, b):
    if a == b and op in "&|":
        return a
    if a.kind == "k" and b.kind == "k":
        return K({"&": a.n & b.n, "|": a.n | b.n, "^": a.n ^ b.n}[op])
    if op == "&" and (b.kind == "k" and b.n == 0 or a.kind == "k" and a.n == 0):
        return K(0)
    if op in "|^" and b.kind == "k" and b.n == 0:
        return a
    if op in "|^" and a.kind == "k" and a.n == 0:
        return b
    if op == "|" and b.kind == "k" and b.n == M32:
        return K(M32)
    if op == "&" and b.kind == "k" and b.n == M32:
        return a
    if op == "&" and b.kind == "k" and a.db < 32 and (b.n >> a.db) == 0:
        return X_("%s & %s" % (a.paren(), cst(b.n)), a, bits=min(a.bits, b.n.bit_length()), db=32)
    if op == "&" and b.kind == "k" and a.bits <= 32 and (b.n >> a.bits) == 0 and b.n == (1 << a.bits) - 1:
        return a
    bits = 32
    if op == "&":
        bits = min(a.bits, b.bits)
    elif a.bits < 32 and b.bits < 32:
        bits = max(a.bits, b.bits)
    return X_("%s %s %s" % (a.paren(), op, b.paren()), a, b, bits=bits)


def v_trunc(v, size):
    """the low `size` bytes, zero-extended"""
    if size == 4:
        return v
    if v.kind == "k":
        return K(v.n & ((1 << 8 * size) - 1))
    if v.bits <= 8 * size and v.db >= 32:
        return v
    return X_("%s & %s" % (v.paren(), cst((1 << 8 * size) - 1)), v, bits=8 * size,
              db=32 if v.db >= 8 * size else v.db)


def v_sext(v, size):
    if size == 4:
        return v
    if v.kind == "k":
        n = v.n & ((1 << 8 * size) - 1)
        if n >> (8 * size - 1):
            n -= 1 << 8 * size
        return K(n)
    t = "int8_t" if size == 1 else "int16_t"
    return X_("(uint32_t)(int32_t)(%s)%s" % (t, v.paren()), v, atom=True, db=32 if v.db >= 8 * size else v.db)


def v_signed(v, size):
    t = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[size]
    if v.kind == "k":
        n = v.n & ((1 << 8 * size) - 1)
        if n >> (8 * size - 1):
            n -= 1 << 8 * size
        return str(n)
    return "(%s)%s" % (t, v.paren())


def v_unsigned(v, size):
    if size == 4 or v.bits <= 8 * size:
        return v.c if v.kind == "k" else v.paren()
    return "(%s)%s" % ({1: "uint8_t", 2: "uint16_t"}[size], v.paren())


def v_merge(old, new, size, high=False):
    """a partial register write: the low `size` bytes (or ah..dh) of old replaced"""
    if size == 4:
        return new
    if high:
        if old.kind == "k" and new.kind == "k":
            return K((old.n & 0xffff00ff) | ((new.n & 0xff) << 8))
        return X_("(%s & 0xffff00ffu) | (%s << 8)" % (old.paren(), v_trunc(new, 1).paren()), old, new)
    m = (1 << 8 * size) - 1
    if old.kind == "k" and new.kind == "k":
        return K((old.n & ~m) | (new.n & m))
    lo = v_trunc(new, size)
    if old.kind == "k" and old.n & ~m & M32 == 0:
        return lo
    return X_("(%s & %s) | %s" % (old.paren(), cst(~m & M32), lo.paren()), old, lo,
              db=max(old.db, 8 * size) if lo.db >= 8 * size else lo.db)


def subst(v, marker, new):
    """replace a host variable (or the held call) in v by the value new"""
    if marker not in v.hv:
        return v
    c = v.c.replace(marker, new.c if new.atom else new.paren())
    w = V(v.kind, c, v.n, (v.reads | new.reads), (v.hv - {marker}) | new.hv, v.bits, v.atom,
          None if v.base is None else subst(v.base, marker, new), min(v.db, new.db))
    return w


# ------------------------------------------------------------------------------------------------ state

class St:
    __slots__ = ("esp", "r", "fl", "pend", "dead", "saved", "held", "bottom", "mach")

    def __init__(self):
        self.esp = 0
        self.r = {}
        self.fl = None
        self.pend = {}         # sp0-relative slot -> V: pushes and argument stores not written yet
        self.dead = {}         # slots popped again: their write is still owed
        self.saved = {}        # sp0-relative slot -> register whose entry value it holds
        self.held = None       # the C text of the last call while its result is unused
        self.bottom = None     # sp0-relative lowest slot of the frame (locals and saved registers)
        self.mach = {}         # register -> the C text the machine's register is known to hold

    def copy(self):
        s = St()
        s.esp, s.r, s.fl, s.pend, s.dead, s.saved, s.held, s.bottom = \
            self.esp, dict(self.r), self.fl, dict(self.pend), dict(self.dead), dict(self.saved), self.held, self.bottom
        s.mach = dict(self.mach)
        return s

    def key(self):
        return (self.esp, tuple(sorted((k, v.key()) for k, v in self.r.items())),
                None if self.fl is None else fl_key(self.fl), tuple(sorted(self.saved.items())), self.bottom,
                tuple(sorted(self.mach.items())))


def fl_key(f):
    return tuple(x.key() if isinstance(x, V) else x for x in f)


REG32 = ["eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"]
SUB = {}
for r32 in ("eax", "ecx", "edx", "ebx"):
    SUB[r32[1] + "l"] = (r32, 1, False)
    SUB[r32[1] + "h"] = (r32, 1, True)
    SUB[r32[1:]] = (r32, 2, False)
for r32 in ("esi", "edi", "ebp", "esp"):
    SUB[r32[1:]] = (r32, 2, False)
for r32 in REG32 + ["esp"]:
    SUB[r32] = (r32, 4, False)

LIVE_REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "fl")


# ------------------------------------------------------------------------------------------------ the lifter

class Lifter:
    def __init__(self, image_path):
        self.img = x2c.Image(image_path)
        self.img_path = image_path
        self.tr = x2c.Translator(self.img, x2c.ghidra_entries(image_path))
        self.conv_cache = {}
        self.call_targets = set()
        self.ported = {int(a, 16): int(f, 0) if f.isdigit() else f for a, f in
                       re.findall(r"(?m)^PORTED\(([0-9a-fA-F]{8}),\s*(\w+)\)", open(os.path.join(ROOT, "src", "ported.h")).read())}

    # ---------------------------------------------------------------- decoding

    def function(self, entry, lifting=False):
        fn = x2c.Fn(self.tr, entry)
        known = self.tr.known
        if lifting:
            self.tr.known = self.call_targets | {entry}
        try:
            self.tr.discover(fn)
        except x2c.Unsupported as e:
            raise Refuse("decode: %s" % e)
        finally:
            self.tr.known = known
        if not lifting:
            for x in fn.ins.values():
                if x.mnemonic == "call" and x.operands[0].type == X.X86_OP_IMM:
                    t = x.operands[0].imm & M32
                    if not self.tr.thunk(t):
                        self.call_targets.add(t)
        return fn

    def scan_calls(self, entries):
        """the targets of every direct call in the image: the real functions (cached in build/)"""
        cache = os.path.join(ROOT, "build", "lift_calls.txt")
        stamp = "%d %d" % (os.path.getsize(self.img_path), len(entries))
        if os.path.exists(cache):
            lines = open(cache).read().split("\n")
            if lines[0] == stamp:
                self.call_targets = {int(t, 16) for t in lines[1].split()}
                return
        for e in entries:
            try:
                self.function(e)
            except Refuse:
                pass
        with open(cache, "w") as f:
            f.write(stamp + "\n" + " ".join("%x" % t for t in sorted(self.call_targets)) + "\n")

    def is_rule(self, fn):
        names = set()
        for x in fn.ins.values():
            if x.mnemonic == "call" and x.operands[0].type == X.X86_OP_IMM:
                n = self.tr.thunk(x.operands[0].imm & M32)
                if n:
                    names.add(n)
        return SETJMP_THUNK_NAME in names and 0x101315e0 in fn.calls

    def callee_conv(self, t):
        """None if t is a plain cdecl function (no register arguments, pops nothing), else why not"""
        if t in self.conv_cache:
            return self.conv_cache[t]
        why = None
        try:
            fn = self.function(t)
            for x in fn.ins.values():
                if x.mnemonic in ("ret", "retn") and x.operands:
                    why = "callee %08x pops its arguments" % t
                    break
            if why is None:
                # register arguments: ecx/edx read before written on the straight line from the entry
                a, written = t, set()
                for _ in range(64):
                    x = fn.ins.get(a)
                    if x is None:
                        break
                    rd, wr = x.regs_access()
                    rd = {SUB.get(x.reg_name(r), (None,))[0] for r in rd}
                    wr = {SUB.get(x.reg_name(r), (None,))[0] for r in wr}
                    if x.mnemonic == "xor" and len(x.operands) == 2 and x.op_str.split(",")[0].strip() == \
                            x.op_str.split(",")[1].strip():
                        rd = set()
                    for r in ("ecx", "edx", "eax"):
                        if r in rd and r not in written:
                            why = "callee %08x reads %s at entry" % (t, r)
                    written |= wr
                    if why or x.mnemonic.startswith("j") or x.mnemonic in ("ret", "call"):
                        break
                    a += x.size
        except Refuse as e:
            why = "callee %08x: %s" % (t, e)
        self.conv_cache[t] = why
        return why

    def blocks(self, fn):
        addrs = sorted(fn.ins)
        leaders = {fn.entry} | set(fn.targets)
        for a in addrs:
            x = fn.ins[a]
            mn = x.mnemonic
            if mn.startswith("j") or mn in ("ret", "retn"):
                leaders.add(a + x.size)
        blocks = collections.OrderedDict()
        cur = None
        for i, a in enumerate(addrs):
            if a in leaders or cur is None:
                cur = [a]
                blocks[a] = cur
            else:
                cur.append(a)
            x = fn.ins[a]
            nxt = a + x.size
            end = x.mnemonic.startswith("j") or x.mnemonic in ("ret", "retn", "int3", "hlt", "ud2")
            if not end and (i + 1 >= len(addrs) or addrs[i + 1] != nxt):
                raise Refuse("code falls off at %08x" % nxt)
            if end:
                cur = None
        return blocks

    def succs(self, fn, b):
        last = fn.ins[b[-1]]
        mn = last.mnemonic
        nxt = last.address + last.size
        if mn in ("ret", "retn"):
            return []
        if mn == "jmp":
            op = last.operands[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & M32
                if t not in fn.ins or (t in self.call_targets and t != fn.entry):
                    raise Refuse("tail jump to %08x" % t)
                return [t]
            if last.address in fn.tables:
                return list(dict.fromkeys(fn.tables[last.address]))
            raise Refuse("indirect jump at %08x" % last.address)
        if mn.startswith("j"):
            if mn in ("jecxz", "jcxz"):
                raise Refuse(mn)
            return [last.operands[0].imm & M32, nxt]
        return [nxt]

    # ---------------------------------------------------------------- liveness

    def use_def(self, x):
        mn = x.mnemonic
        if mn == "call":
            return set(), {"eax", "ecx", "edx", "fl"}
        if mn in ("ret", "retn"):
            return {"eax", "ebx", "esi", "edi"}, set()
        rd, wr = x.regs_access()
        use, df = set(), set()
        for r in rd:
            n = x.reg_name(r)
            if n == "eflags":
                use.add("fl")
            elif n in SUB:
                use.add(SUB[n][0])
        for r in wr:
            n = x.reg_name(r)
            if n == "eflags":
                df.add("fl")
            elif n in SUB:
                g, size, _ = SUB[n]
                df.add(g)
        ops = x.operands
        if mn in ("xor", "sub") and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG \
                and ops[0].reg == ops[1].reg:
            use.discard(SUB[x.reg_name(ops[0].reg)][0])
        if mn in ("inc", "dec"):
            use.discard("fl")          # CF is kept: a later use of it is refused where it happens
        if mn in ("cmp", "test") and "fl" in use:
            use.discard("fl")
        use.discard("esp")
        df.discard("esp")
        use.discard("ebp")
        df.discard("ebp")
        return use & set(LIVE_REGS), df & set(LIVE_REGS)

    def liveness(self, fn, blocks, succ):
        ud = {}
        for b in blocks.values():
            for a in b:
                ud[a] = self.use_def(fn.ins[a])
        live_in = {s: set() for s in blocks}
        live_out = {s: set() for s in blocks}
        changed = True
        while changed:
            changed = False
            for s in reversed(list(blocks)):
                out = set()
                for t in succ[s]:
                    out |= live_in[t]
                live = set(out)
                for a in reversed(blocks[s]):
                    u, d = ud[a]
                    live = (live - d) | u
                if out != live_out[s] or live != live_in[s]:
                    live_out[s], live_in[s] = out, live
                    changed = True
        after = {}
        for s, b in blocks.items():
            live = set(live_out[s])
            for a in reversed(b):
                after[a] = frozenset(live)
                u, d = ud[a]
                live = (live - d) | u
        return live_in, after

    # ---------------------------------------------------------------- lifting one function

    def lift(self, entry):
        fn = self.function(entry)
        if not self.is_rule(fn):
            raise Refuse("not a rule function")
        fn = self.function(entry, lifting=True)
        return FnLifter(self, fn).run()


class FnLifter:
    def __init__(self, L, fn):
        self.L, self.fn = L, fn
        self.tmpn = 0
        self.out = None
        self.params = set()
        self.roles = {}            # fp offset -> (role name, priority)
        self.fp_used = set()
        self.setjmps = 0
        self.returns_join = set()
        self.saved_offs = set()
        self.bottom = None
        self.imports = set()

    # ---------------------------------------------------------------- helpers

    def refuse(self, x, why):
        raise Refuse("%s at %08x (%s %s)" % (why, x.address, x.mnemonic, x.op_str))

    def emit(self, line):
        if self.out is not None:
            self.out.append(line)

    def temp(self, st, v, x):
        """materialize v into a new temporary"""
        name = "@T%x_%d@" % (x.address, self.tmpk)
        self.tmpk += 1
        self.emit("%s = %s;" % (name, v.c))
        for r in [r for r, t in st.mach.items() if name in t]:
            del st.mach[r]
        return V("x", name, hv={name}, bits=v.bits, atom=True)

    def full(self, v, x, bits=32):
        """v is used with `bits` bits: they must be defined"""
        if v.db < bits:
            self.refuse(x, "use of a value with undefined upper bits")
        return v

    def reg_get(self, st, name, x, allow_held=False):
        g, size, high = SUB[name]
        if g == "esp":
            self.refuse(x, "esp used as a value")
        v = st.r[g]
        if v.kind == "u":
            self.refuse(x, "read of undefined %s" % g)
        if "@H@" in v.hv and not allow_held:
            self.flush_held(st, x, force=True)
            v = st.r[g]
        if size == 4:
            return v
        if high:
            if v.kind == "k":
                return K((v.n >> 8) & 0xff)
            return X_("(%s >> 8) & 0xffu" % v.paren(), v, bits=8)
        return v_trunc(v, size)

    def reg_set(self, st, name, v, x):
        g, size, high = SUB[name]
        if g in ("esp", "ebp"):
            self.refuse(x, "write to %s" % g)
        if size == 4:
            st.r[g] = v
            return
        old = st.r[g]
        if "@H@" in old.hv:
            self.flush_held(st, x, force=True)
            old = st.r[g]
        if old.kind == "u":
            # the upper bits stay undefined: only the low part may be used (checked where values are used)
            if high:
                self.refuse(x, "write to %s of an undefined register" % name)
            lo = v_trunc(v, size)
            st.r[g] = V("x", lo.c, reads=lo.reads, hv=lo.hv, bits=8 * size, atom=lo.atom, db=8 * size)
            return
        st.r[g] = v_merge(old, v, size, high)

    def addr(self, st, x, op, partial=False):
        m = op.mem
        if m.segment:
            self.refuse(x, "segment prefix")
        disp = m.disp
        parts = []
        if m.base:
            b = x.reg_name(m.base)
            if b == "esp":
                v = FP(st.esp + 4)
            elif b == "ebp":
                v = st.r["ebp"]
                if v.kind != "fp":
                    self.refuse(x, "ebp is not the frame pointer")
            else:
                v = self.reg_get(st, b, x)
                if not partial:
                    self.full(v, x)
            parts.append(v)
        if m.index:
            iv = self.reg_get(st, x.reg_name(m.index), x)
            if not partial:
                self.full(iv, x)
            if m.scale != 1:
                iv = self.mul_const(iv, m.scale)
            parts.append(iv)
        v = K(disp & M32)
        for p in parts:
            v = v_add(p, v)
        return v

    def mul_const(self, v, k):
        if v.kind == "k":
            return K(v.n * k)
        return X_("%s * %s" % (v.paren(), cst(k)), v)

    def mem_kind(self, a, size):
        """what a read at address a is: ('f', off, size) in the frame, else ('m',)"""
        if a.kind == "fp":
            return ("f", a.n, size)
        return ("m",)

    def check_param_slot(self, a, x, write):
        if a.kind == "fp" and a.n >= 8:
            if write:
                self.refuse(x, "write to a parameter slot")
            return (a.n - 8) // 4 if (a.n - 8) % 4 == 0 else None
        return None

    def load(self, st, x, op, size=None):
        size = size or op.size
        a = self.addr(st, x, op)
        if a.kind == "fp":
            self.fp_used.add(a.n)
            # a slot of the pending pushes: write them first
            self.flush_region(st, x, a.n - 4, size)
        pi = self.check_param_slot(a, x, False)
        if pi is not None and size == 4:
            self.params.add(pi)
            return V("p", "@P%d@" % pi, pi, atom=True)
        if a.kind == "fp" and a.n >= 4:
            self.refuse(x, "read of the return address or a parameter part")
        fnm = {1: "rd8", 2: "rd16", 4: "rd32"}[size]
        return X_("%s(c, %s)" % (fnm, a.c), a, reads={self.mem_kind(a, size)}, bits=8 * size, atom=True)

    def store(self, st, x, op, v, size=None, live=frozenset()):
        size = size or op.size
        a = self.addr(st, x, op)
        self.check_param_slot(a, x, True)
        if a.kind == "fp":
            self.fp_used.add(a.n)
            slot = a.n - 4
            if a.n >= 0:
                self.refuse(x, "write to the saved ebp or above")
            # an argument slot below the frame: it becomes a pending argument
            if size == 4 and st.bottom is not None and slot < st.bottom and slot >= st.esp and slot % 4 == 0:
                self.full(v, x)
                st.dead.pop(slot, None)
                self.effect(st, x, live, write=None, exclude_pend=True, nowrite=True)
                st.pend[slot] = v
                return
            self.flush_region(st, x, slot, size)
            for s in list(st.saved):
                if s < slot + size and slot < s + 4:
                    del st.saved[s]
        if is_undef(v):
            self.refuse(x, "store of an undefined value")
        self.full(v, x, 8 * size)
        self.effect(st, x, live, write=(a, size))
        fnm = {1: "wr8", 2: "wr16", 4: "wr32"}[size]
        val = v_trunc(v, size) if size < 4 else v
        cast = {1: "(uint8_t)", 2: "(uint16_t)", 4: ""}[size]
        if size < 4 and v.bits > 8 * size:
            val = V("x", cast + v.paren(), atom=True)
        self.emit("%s(c, %s, %s);" % (fnm, a.c, val.c))

    def flush_region(self, st, x, lo, size):
        """write the pending pushes (and owed writes) that overlap sp0-relative [lo, lo+size)"""
        hit = [s for s in list(st.pend) + list(st.dead) if s < lo + size and lo < s + 4]
        if hit:
            self.write_slots(st, x, sorted(set(hit)))

    @staticmethod
    def slot_conflict(v, s):
        """v reads the stack slot s (sp0-relative)? (a pending argument slot is below every local and not
        addressable before its call: only a read of those very frame bytes can see its write)"""
        for rd in v.reads:
            if rd[0] == "f" and rd[1] < s + 4 + 4 and s + 4 < rd[1] + rd[2]:
                return True
        return False

    def write_slots(self, st, x, slots):
        for s in slots:
            for r, w in list(st.r.items()):
                if w.lazy and self.slot_conflict(w, s):
                    st.r[r] = self.temp(st, w, x)
            if st.fl is not None and any(isinstance(q, V) and self.slot_conflict(q, s) for q in st.fl):
                st.fl = tuple(self.temp(st, q, x) if isinstance(q, V) and self.slot_conflict(q, s) else q
                              for q in st.fl)
            for d in (st.pend, st.dead):
                for k in list(d):
                    if k != s and d[k].lazy and self.slot_conflict(d[k], s):
                        d[k] = self.temp(st, d[k], x)
            v = st.pend.pop(s, None)
            if v is None:
                v = st.dead.pop(s)
            if is_undef(v):
                self.refuse(x, "pushed an undefined value")
            if "@H@" in v.hv:
                self.flush_held(st, x, force=True)
                v = subst(v, "@H@", self.held_temp)
            self.fp_used.add(s + 4)
            self.emit("wr32(c, %s, %s);" % (FP(s + 4).c, v.c))

    # ---------------------------------------------------------------- side effects

    def flush_held(self, st, x, force=False, live=None):
        """the held call is emitted: into a temporary if its result is still used, else as a statement"""
        if st.held is None:
            return None
        used = force
        if not used:
            for r, v in st.r.items():
                if "@H@" in v.hv and (live is None or r in live):
                    used = True
            if st.fl is not None and any(isinstance(p, V) and "@H@" in p.hv for p in st.fl) and \
                    (live is None or "fl" in live):
                used = True
            for v in list(st.pend.values()) + list(st.dead.values()):
                if "@H@" in v.hv:
                    used = True
        call = st.held
        st.held = None
        if used:
            name = "@T%x_%d@" % (x.address, self.tmpk)
            self.tmpk += 1
            self.emit("%s = %s;" % (name, call))
            t = V("x", name, hv={name}, atom=True)
            self.held_temp = t
            self.replace_everywhere(st, "@H@", t)
            return t
        self.emit("%s;" % call)
        self.replace_everywhere(st, "@H@", UNDEF)
        return None

    def replace_everywhere(self, st, marker, new):
        for r, v in list(st.r.items()):
            if marker in v.hv:
                st.r[r] = UNDEF if new is UNDEF else subst(v, marker, new)
        if st.fl is not None and any(isinstance(p, V) and marker in p.hv for p in st.fl):
            st.fl = None if new is UNDEF else tuple(subst(p, marker, new) if isinstance(p, V) else p for p in st.fl)
        for d in (st.pend, st.dead):
            for s, v in list(d.items()):
                if marker in v.hv:
                    if new is UNDEF:
                        raise Refuse("held value lost")
                    d[s] = subst(v, marker, new)

    def conflicts(self, v, write):
        if not v.reads:
            return False
        if write is None:
            return True                 # a call: memory and registers may change
        a, size = write
        for rd in v.reads:
            if rd[0] == "r":
                continue
            if rd[0] == "m":
                return True
            if a.kind != "fp":
                return True
            off, sz = rd[1], rd[2]
            if off < a.n + size and a.n < off + sz:
                return True
        return False

    def effect(self, st, x, live, write=None, exclude_pend=False, nowrite=False, keep=()):
        """before a memory write (write = (address, size)) or a call (write None): the held call first, then
        every live lazy value the effect could change is evaluated into a temporary"""
        self.flush_held(st, x, live=live)
        if nowrite:
            return
        for r in list(st.r):
            v = st.r[r]
            if v.lazy and self.conflicts(v, write):
                if r in live and r not in keep:
                    st.r[r] = self.temp(st, v, x)
                else:
                    st.r[r] = UNDEF
        if st.fl is not None:
            if "fl" not in live:
                st.fl = None
            elif any(isinstance(p, V) and self.conflicts(p, write) for p in st.fl):
                st.fl = tuple(self.temp(st, p, x) if isinstance(p, V) and self.conflicts(p, write) else p
                              for p in st.fl)
        if not exclude_pend:
            for d in (st.pend, st.dead):
                for s in sorted(d):
                    v = d[s]
                    if v.lazy and self.conflicts(v, write):
                        d[s] = self.temp(st, v, x)

    # ---------------------------------------------------------------- flags and conditions

    def cond(self, st, cc, x):
        f = st.fl
        if f is None:
            self.refuse(x, "flags unknown")
        kind, size, a, b, r = f
        for p in (a, b, r):
            if p is not None:
                self.full(p, x, 8 * size)
        s = cc
        if kind == "sub":
            A, B = a, b
            ua, ub = v_unsigned(A, size), v_unsigned(B, size)
            sa, sb = v_signed(A, size), v_signed(B, size)
            if s in ("e", "z"):
                return "%s == %s" % (ua, ub)
            if s in ("ne", "nz"):
                return "%s != %s" % (ua, ub)
            m = {"b": "<", "c": "<", "nae": "<", "ae": ">=", "nb": ">=", "nc": ">=", "be": "<=", "na": "<=",
                 "a": ">", "nbe": ">"}
            if s in m:
                return "%s %s %s" % (ua, m[s], ub)
            m = {"l": "<", "nge": "<", "ge": ">=", "nl": ">=", "le": "<=", "ng": "<=", "g": ">", "nle": ">"}
            if s in m:
                return "%s %s %s" % (sa, m[s], sb)
            if s in ("s", "ns"):
                return "%s %s 0" % (v_signed(r, size), "<" if s == "s" else ">=")
            self.refuse(x, "condition %s after a compare" % s)
        if kind == "sahf":
            # CF bit 0, PF bit 2, ZF bit 6 of ah
            m = {"b": 1, "c": 1, "nae": 1, "e": 0x40, "z": 0x40, "p": 4, "pe": 4, "be": 0x41, "na": 0x41}
            n = {"ae": 1, "nb": 1, "nc": 1, "ne": 0x40, "nz": 0x40, "np": 4, "po": 4, "a": 0x41, "nbe": 0x41}
            if s in m:
                return "(%s & 0x%xu) != 0" % (r.paren(), m[s])
            if s in n:
                return "(%s & 0x%xu) == 0" % (r.paren(), n[s])
            self.refuse(x, "condition %s after sahf" % s)
        rv = r
        ur, sr = v_unsigned(rv, size), v_signed(rv, size)
        if s in ("e", "z"):
            return "%s == 0" % ur
        if s in ("ne", "nz"):
            return "%s != 0" % ur
        if s in ("s", "ns"):
            return "%s %s 0" % (sr, "<" if s == "s" else ">=")
        if kind == "logic":
            m = {"l": "<", "nge": "<", "ge": ">=", "nl": ">=", "le": "<=", "ng": "<=", "g": ">", "nle": ">"}
            if s in m:
                return "%s %s 0" % (sr, m[s])
            if s in ("be", "na"):
                return "%s == 0" % ur
            if s in ("a", "nbe"):
                return "%s != 0" % ur
            if s in ("b", "c", "nae"):
                return "0"
            if s in ("ae", "nb", "nc"):
                return "1"
        if kind == "neg":
            if s in ("b", "c", "nae"):
                return "%s != 0" % ur
            if s in ("ae", "nb", "nc"):
                return "%s == 0" % ur
        if kind == "add" and a is not None:
            if s in ("b", "c", "nae", "ae", "nb", "nc"):
                c_ = "%s < %s" % (ur, v_unsigned(a, size))
                return c_ if s in ("b", "c", "nae") else "!(%s)" % c_
        self.refuse(x, "condition %s after %s" % (s, kind))

    def simplify_cond(self, c):
        """(t + k) == 0 -> t == -k (the backtracking dispatch); f(..) != 0 -> f(..)"""
        m = re.match(r"^(.*) (!=|==) 0$", c)
        if m and self.atomic(m.group(1)) and not re.match(r"^\((.*) [-+] (0x[0-9a-f]+u|\d+u)\)$", m.group(1)):
            return m.group(1) if m.group(2) == "!=" else "!" + m.group(1)
        m = re.match(r"^\((.*) - (0x[0-9a-f]+u|\d+u)\) (==|!=) 0$", c)
        if m and "(" not in m.group(1):
            return "%s %s %s" % (m.group(1), m.group(3), m.group(2))
        m = re.match(r"^\((.*) \+ (0x[0-9a-f]+u|\d+u)\) (==|!=) 0$", c)
        if m and "(" not in m.group(1):
            n = (-int(m.group(2)[:-1], 0)) & M32
            return "%s %s %s" % (m.group(1), m.group(3), cst(n))
        return c

    @staticmethod
    def atomic(e):
        """e is one call, name or parenthesized expression"""
        if re.match(r"^[@\w]+(->\w+)?$", e):
            return True
        if not e.endswith(")"):
            return False
        depth = 0
        start = e.index("(") if re.match(r"^[\w@]*\(", e) else -1
        if start < 0:
            return False
        for i, ch in enumerate(e):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and i != len(e) - 1:
                    return False
        return True

    # ---------------------------------------------------------------- one instruction

    def step(self, st, x, live):
        # what must survive this instruction: live after it, less what it defines itself
        live = frozenset(live - self.L.use_def(x)[1])
        mn, ops = x.mnemonic, x.operands
        b = x.bytes
        k = 0
        while b[k] in (0x66, 0x67, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x9b):
            k += 1
        if mn in ("fwait", "wait"):
            return
        if 0xd8 <= b[k] <= 0xdf:
            self.x87(st, x, live, b[k], b[k + 1])
            return
        if mn.startswith("rep") or mn in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "scasb", "scasw",
                                          "scasd", "cmpsb", "cmpsw", "cmpsd", "lodsb", "lodsw", "lodsd"):
            self.refuse(x, "string instruction")

        def src(o, size=None, allow_held=False):
            if o.type == X.X86_OP_REG:
                return self.reg_get(st, x.reg_name(o.reg), x, allow_held)
            if o.type == X.X86_OP_IMM:
                sz = size or o.size
                return K(o.imm & ((1 << 8 * sz) - 1) if sz < 4 else o.imm)
            return self.load(st, x, o, size)

        def dst(o, v):
            if o.type == X.X86_OP_REG:
                self.reg_set(st, x.reg_name(o.reg), v, x)
            else:
                self.store(st, x, o, v, live=live)

        if mn in ("nop",):
            return
        if mn == "push":
            o = ops[0]
            if o.type == X.X86_OP_REG and o.size != 4 or o.type == X.X86_OP_MEM and o.size != 4:
                self.refuse(x, "push of %d bytes" % o.size)
            if o.type == X.X86_OP_REG and x.reg_name(o.reg) == "esp":
                self.refuse(x, "push esp")
            if o.type == X.X86_OP_REG and x.reg_name(o.reg) == "ebp" and st.r["ebp"].kind == "in":
                v = st.r["ebp"]
            elif o.type == X.X86_OP_REG and st.r[SUB[x.reg_name(o.reg)][0]].kind == "u":
                v = UNDEF
            else:
                v = src(o, 4)
            st.esp -= 4
            slot = st.esp
            st.dead.pop(slot, None)
            if v.kind == "in":
                # a callee-saved register saved in the frame (the prologue)
                self.fp_used.add(slot + 4)
                self.emit("wr32(c, %s, %s);  /* saved %s */" % (FP(slot + 4).c, "c->" + v.n, v.n))
                self.saved_offs.add(slot + 4)
                st.saved[slot] = v.n
                st.bottom = slot if st.bottom is None else min(st.bottom, slot)
                self.bottom = st.bottom if self.bottom is None else min(self.bottom, st.bottom)
                return
            if is_undef(v) and o.type == X.X86_OP_REG and v.kind == "u":
                g = SUB[x.reg_name(o.reg)][0]
                st.pend[slot] = X_("c->%s" % g, reads={("r",)}, atom=True)
                return
            if is_undef(v):
                self.refuse(x, "push of an undefined value")
            st.pend[slot] = self.full(v, x)
            return
        if mn == "pop":
            o = ops[0]
            slot = st.esp
            if slot in st.pend:
                v = st.pend.pop(slot)
                st.dead[slot] = v
            elif slot in st.saved:
                v = V("in", "in_" + st.saved[slot], st.saved[slot], atom=True)
            else:
                v = X_("rd32(c, %s)" % FP(slot + 4).c, reads={("f", slot + 4, 4)}, atom=True)
                self.fp_used.add(slot + 4)
                if slot + 4 >= 4:
                    self.refuse(x, "pop above the frame")
            st.esp += 4
            if o.type == X.X86_OP_REG and x.reg_name(o.reg) == "ebp":
                if v.kind != "in" or v.n != "ebp":
                    self.refuse(x, "pop ebp of something else")
                st.r["ebp"] = v
                return
            if o.type != X.X86_OP_REG:
                self.refuse(x, "pop to memory")
            if v.kind == "in" and v.n != x.reg_name(o.reg):
                self.refuse(x, "saved register restored into another")
            dst(o, v)
            return
        if mn == "leave":
            if st.r["ebp"].kind != "fp" or st.r["ebp"].n != 0:
                self.refuse(x, "leave without the frame")
            if st.pend or st.dead:
                self.flush_held(st, x, live=live)
                self.write_slots(st, x, sorted(set(st.pend) | set(st.dead)))
            st.esp = -4
            if st.saved.get(-4) != "ebp":
                self.refuse(x, "saved ebp lost")
            st.r["ebp"] = V("in", "in_ebp", "ebp", atom=True)
            st.esp = 0
            return
        if mn == "mov":
            d, s_ = ops
            if d.type == X.X86_OP_REG and x.reg_name(d.reg) == "ebp":
                if s_.type == X.X86_OP_REG and x.reg_name(s_.reg) == "esp" and st.esp == -4 and \
                        st.r["ebp"].kind == "in":
                    st.r["ebp"] = FP(0)
                    self.emit("c->ebp = fp;")
                    st.mach["ebp"] = "fp"
                    return
                self.refuse(x, "write to ebp")
            if s_.type == X.X86_OP_REG and x.reg_name(s_.reg) == "esp":
                self.refuse(x, "esp copied")
            dst(d, src(s_, d.size))
            return
        if mn in ("movzx", "movsx"):
            v = src(ops[1])
            v = v_trunc(v, ops[1].size) if mn == "movzx" else v_sext(v, ops[1].size)
            dst(ops[0], v)
            return
        if mn == "lea":
            dst(ops[0], self.addr(st, x, ops[1], partial=True))
            return
        if mn in ("add", "sub") and ops[0].type == X.X86_OP_REG and x.reg_name(ops[0].reg) == "esp":
            if ops[1].type != X.X86_OP_IMM:
                self.refuse(x, "esp adjusted by a register")
            n = ops[1].imm if mn == "add" else -ops[1].imm
            new = st.esp + n
            if n > 0:
                # the popped slots: pending pushes that are dropped still have been written by the original
                for s in list(st.pend):
                    if s < new:
                        st.dead[s] = st.pend.pop(s)
            st.esp = new
            if n < 0 and st.bottom is None:
                st.bottom = new
            elif n < 0:
                st.bottom = min(st.bottom, new)
            if n < 0:
                self.bottom = st.bottom if self.bottom is None else min(self.bottom, st.bottom)
            st.fl = None if "fl" not in live else self.refuse(x, "flags of an esp adjustment")
            return
        if mn in ("add", "sub", "and", "or", "xor", "cmp", "test", "adc", "sbb"):
            d, s_ = ops
            size = d.size
            if mn in ("xor", "sub") and d.type == X.X86_OP_REG and s_.type == X.X86_OP_REG and d.reg == s_.reg:
                dst(d, K(0))
                st.fl = ("logic", size, None, None, K(0))
                return
            if mn == "sbb" and d.type == X.X86_OP_REG and s_.type == X.X86_OP_REG and d.reg == s_.reg and size == 4:
                # all ones when CF is set, else zero
                r = X_("(%s) ? 0xffffffffu : 0u" % self.cond(st, "b", x), *[p for p in st.fl if isinstance(p, V)])
                dst(d, r)
                st.fl = None if "fl" not in live else self.refuse(x, "flags of sbb")
                return
            if mn in ("adc", "sbb"):
                self.refuse(x, mn)
            if d.type == X.X86_OP_REG and x.reg_name(d.reg) in ("esp", "ebp"):
                self.refuse(x, "arithmetic on %s" % x.reg_name(d.reg))
            a = src(d, allow_held=mn in ("cmp", "test"))
            bb = src(s_, size, allow_held=mn in ("cmp", "test"))
            if mn == "cmp":
                st.fl = ("sub", size, a, bb, v_trunc(v_sub(a, bb), size))
                return
            if mn == "test":
                r = v_logic("&", a, bb)
                st.fl = ("logic", size, None, None, v_trunc(r, size))
                return
            if mn == "add":
                r = v_add(a, bb)
                fl = ("add", size, a, None, v_trunc(r, size))
            elif mn == "sub":
                r = v_sub(a, bb)
                fl = ("sub", size, a, bb, v_trunc(r, size))
            else:
                r = v_logic({"and": "&", "or": "|", "xor": "^"}[mn], a, bb)
                fl = ("logic", size, None, None, v_trunc(r, size))
            if d.type == X.X86_OP_MEM:
                self.store(st, x, d, r, live=live)
                if "fl" in live:
                    if fl[0] != "logic":
                        self.refuse(x, "flags of a memory %s" % mn)
                    # the result is the memory's new content
                    fl = ("logic", size, None, None, self.load(st, x, d))
            else:
                dst(d, r if size == 4 else r)
            st.fl = fl
            return
        if mn in ("inc", "dec", "neg", "not"):
            d = ops[0]
            size = d.size
            a = src(d)
            if mn == "inc":
                r = v_add(a, K(1))
            elif mn == "dec":
                r = v_add(a, K(M32))
            elif mn == "neg":
                r = v_sub(K(0), a)
            else:
                r = v_logic("^", a, K((1 << 8 * size) - 1))
            if d.type == X.X86_OP_MEM:
                self.store(st, x, d, r, live=live)
                if "fl" in live and mn != "not":
                    self.refuse(x, "flags of a memory %s" % mn)
            else:
                dst(d, r)
            if mn != "not":
                st.fl = (mn, size, None, None, v_trunc(r, size))
            return
        if mn in ("shl", "sal", "shr", "sar"):
            d = ops[0]
            size = d.size
            if len(ops) < 2 or ops[1].type != X.X86_OP_IMM:
                self.refuse(x, "shift by a register")
            n = ops[1].imm & 31
            a = src(d)
            if n == 0:
                return
            if mn in ("shl", "sal"):
                r = K(a.n << n) if a.kind == "k" else X_("%s << %d" % (a.paren(), n), a)
            elif mn == "shr":
                self.full(a, x, 8 * size)
                a = v_trunc(a, size)
                r = K(a.n >> n) if a.kind == "k" else X_("%s >> %d" % (a.paren(), n), a, bits=max(1, a.bits - n))
            else:
                if size < 4:
                    self.full(a, x, 8 * size)
                r = X_("(uint32_t)(%s >> %d)" % (v_signed(a, size), n), a, atom=True)
            if d.type == X.X86_OP_MEM:
                self.refuse(x, "shift in memory")
            dst(d, r)
            st.fl = ("shift", size, None, None, v_trunc(r, size))
            return
        if mn == "imul":
            if len(ops) == 1:
                self.refuse(x, "one-operand imul")
            if len(ops) == 2:
                a, bb = src(ops[0]), src(ops[1])
            else:
                a, bb = src(ops[1]), src(ops[2], ops[0].size)
            if ops[0].size == 1:
                self.refuse(x, "imul of %d bytes" % ops[0].size)
            if bb.kind == "k":
                r = self.mul_const(a, bb.n) if a.kind != "k" else K(a.n * bb.n)
            else:
                r = X_("%s * %s" % (a.paren(), bb.paren()), a, bb)
            dst(ops[0], r)
            st.fl = None if "fl" not in live else self.refuse(x, "flags of imul")
            return
        if mn == "sahf":
            ah = self.reg_get(st, "ah", x)
            st.fl = ("sahf", 1, None, None, ah)
            return
        if mn == "cdq":
            a = self.reg_get(st, "eax", x)
            st.r["edx"] = X_("(uint32_t)((int32_t)%s >> 31)" % self.full(a, x).paren(), a, atom=True)
            return
        if mn == "xchg":
            a, bb = src(ops[0]), src(ops[1])
            if ops[0].type != X.X86_OP_REG or ops[1].type != X.X86_OP_REG:
                self.refuse(x, "xchg with memory")
            dst(ops[0], bb)
            dst(ops[1], a)
            return
        if mn.startswith("set"):
            c_ = self.cond(st, mn[3:], x)
            dst(ops[0], X_(c_, *[p for p in st.fl if isinstance(p, V)], bits=1))
            return
        if mn == "call":
            self.call(st, x, live)
            return
        self.refuse(x, "instruction %s" % mn)

    # ---------------------------------------------------------------- x87

    # the memory an x87 instruction writes: (opcode, reg field) -> bytes
    X87_STORES = {(0xd9, 2): 4, (0xd9, 3): 4, (0xd9, 7): 2, (0xdb, 2): 4, (0xdb, 3): 4, (0xdb, 7): 10,
                  (0xdd, 2): 8, (0xdd, 3): 8, (0xdd, 7): 2, (0xdf, 2): 2, (0xdf, 3): 2, (0xdf, 7): 8}
    X87_LOADS = {0xd8: 4, 0xd9: 4, 0xda: 4, 0xdb: 4, 0xdc: 8, 0xdd: 8, 0xde: 2, 0xdf: 2}

    def x87(self, st, x, live, op, modrm):
        """an x87 instruction works on the machine's x87 (exact 80-bit, src/fx80.c) as x2c emits it; only its
        memory operand's address is the lifted one"""
        mod, reg = modrm >> 6, (modrm >> 3) & 7
        self.flush_held(st, x, live=live)
        if op == 0xdf and modrm == 0xe0:            # fnstsw ax
            t = self.temp(st, X_("fsw(c)", reads={("r",)}, atom=True, bits=16), x)
            self.reg_set(st, "ax", t, x)
            return
        a = None
        if mod != 3:
            mem = [o for o in x.operands if o.type == X.X86_OP_MEM]
            if not mem:
                self.refuse(x, "x87 operand")
            a = self.addr(st, x, mem[0])
            store = self.X87_STORES.get((op, reg))
            if a.kind == "fp":
                if a.n >= 0:
                    self.refuse(x, "x87 access above the frame")
                self.fp_used.add(a.n)
                size = store or (10 if (op, reg) in ((0xdb, 5),) else 8 if (op, reg) == (0xdf, 5) else
                                 self.X87_LOADS[op] if not (op == 0xd9 and reg in (5, 7)) else 2)
                self.flush_region(st, x, a.n - 4, size)
            if store:
                self.full(a, x)
                self.effect(st, x, live, write=(a, store))
                if a.kind == "fp":
                    for s_ in list(st.saved):
                        if s_ < a.n - 4 + store and a.n - 4 < s_ + 4:
                            del st.saved[s_]

        class Addr:
            @staticmethod
            def addr(_x, _op):
                return a.c

        try:
            lines = self.L.tr.x87(Addr, x, op, modrm)
        except x2c.Unsupported as e:
            self.refuse(x, "x87 %s" % e)
        for l in lines:
            if "c->e" in l:
                self.refuse(x, "x87 touching the registers")
            self.emit(l)

    # ---------------------------------------------------------------- calls

    def machine_regs(self, st, x, regs):
        """the machine's registers made what the original has in them here (before a call, a setjmp, a
        return): a callee's prologue saves them and MSVC's `push ecx` reserves a local with them, so the
        stack bytes it leaves are then the original's. An undefined register (after a call) already is."""
        # values that read a machine register other than their own are evaluated first
        for r in list(st.r):
            v = st.r[r]
            if "c->e" in v.c and v.c != "c->" + r:
                st.r[r] = self.temp(st, v, x)
        if st.fl is not None:
            st.fl = tuple(self.temp(st, p, x) if isinstance(p, V) and "c->e" in p.c else p for p in st.fl)
        for r in regs:
            v = st.r.get(r, UNDEF)
            if r == "ebp":
                t = "fp" if v.kind == "fp" and v.n == 0 else "in_ebp" if v.kind == "in" else None
                if t is None:
                    self.refuse(x, "ebp unknown at a call")
            elif v.kind == "u" or is_undef(v) or "@H@" in v.hv or v.c == "c->" + r:
                continue
            elif v.db >= 32:
                t = v.c
            elif v.db > 0:
                m = (1 << v.db) - 1
                t = "(c->%s & %s) | (%s & %s)" % (r, cst(~m & M32), v.paren(), cst(m))
            else:
                continue
            if st.mach.get(r) == t:
                continue
            self.emit("c->%s = %s;" % (r, t))
            # join variables are assigned on edges, a partial merge reads the register: not remembered
            if "@J" in t or "c->" in t:
                st.mach.pop(r, None)
            else:
                st.mach[r] = t

    def clobbered(self, st):
        """after a call: the machine's scratch registers are the callee's"""
        for r in ("eax", "ecx", "edx"):
            st.mach.pop(r, None)

    def unmachine(self, st, x, v):
        """v as a temporary if it reads a machine register (about to be rewritten)"""
        return self.temp(st, v, x) if "c->e" in v.c else v

    def call(self, st, x, live):
        op = x.operands[0]
        ret = x.address + x.size
        if op.type != X.X86_OP_IMM:
            if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and \
                    (op.mem.disp & M32) in self.L.img.imports:
                self.refuse(x, "call through the import table")
            self.refuse(x, "indirect call")
        t = op.imm & M32
        name = self.L.tr.thunk(t)
        sp = st.esp               # sp0-relative esp at the call (the arguments are above)
        off = -(sp + 4)           # the S() offset: sp0 + sp = fp + 4 + sp = fp - off
        self.flush_held(st, x, live=live)
        if name == SETJMP_THUNK_NAME:
            args = self.take_args(st, x, 2, live)
            self.effect(st, x, live, write=None)
            self.machine_regs(st, x, ALL_REGS)
            # after a longjmp only what is pure may be used again
            for r in ("ebx", "esi", "edi"):
                v = st.r[r]
                if r in live and not v.pure:
                    self.refuse(x, "%s not pure at the setjmp" % r)
            self.emit("GUEST_SETJMP(fp - 0x%x, 0x%x, %s, %s);" % (off, ret, args[0].c, args[1].c))
            self.clobbered(st)
            self.jmpbuf = args[0]
            if args[0].kind == "fp":
                self.role(args[0].n, "jb", 9)
            st.r["eax"] = X_("c->eax", reads={("r",)}, atom=True)
            st.r["ecx"] = UNDEF
            st.r["edx"] = UNDEF
            st.fl = None
            return
        if name:
            if name not in IMPORTS_CDECL:
                self.refuse(x, "call of import %s" % name)
            nargs = 0
            while (sp + 4 * nargs) in st.pend:
                nargs += 1
            args = [self.unmachine(st, x, a) for a in self.take_args(st, x, nargs, live)]
            self.machine_regs(st, x, ALL_REGS)
            self.effect(st, x, live, write=None, exclude_pend=True)
            fname = "imp_" + x2c.import_fn_name(name)
            self.imports.add(fname)
            st.held = self.call_text(off, ret, fname, [a.c if a.kind != "k" else self.arg_text(0, 9, a) for a in args])
            self.clobbered(st)
            st.r["eax"] = V("x", "@H@", hv={"@H@"}, atom=True)
            st.r["ecx"] = UNDEF
            st.r["edx"] = UNDEF
            st.fl = None
            return
        if t in self.L.ported:
            conv = None if t in RUNTIME else self.L.callee_conv(t)
        else:
            conv = self.L.callee_conv(t)
        if conv:
            self.refuse(x, conv)
        if t in RUNTIME:
            rname, nargs = RUNTIME[t]
        else:
            rname, nargs = None, None
        if nargs is None:
            # the pushes since the last call (consecutive slots from esp up)
            nargs = 0
            while (sp + 4 * nargs) in st.pend:
                nargs += 1
        args = [self.unmachine(st, x, a) for a in self.take_args(st, x, nargs, live)]
        self.machine_regs(st, x, ALL_REGS)
        self.effect(st, x, live, write=None, exclude_pend=True)
        self.note_roles(t, args)
        sargs = ", ".join(self.arg_text(t, i, a) for i, a in enumerate(args))
        site = "S(0x%x, 0x%x)" % (off, ret)
        if rname:
            text = "r_%s(%s%s)" % (rname, site, ", " + sargs if sargs else "")
        else:
            text = self.call_text(off, ret, "f_%08x" % t, [self.arg_text(t, i, a) for i, a in enumerate(args)])
        st.held = text
        self.clobbered(st)
        st.r["eax"] = V("x", "@H@", hv={"@H@"}, atom=True)
        st.r["ecx"] = UNDEF
        st.r["edx"] = UNDEF
        st.fl = None

    @staticmethod
    def call_text(off, ret, fname, sargs):
        site = "S(0x%x, 0x%x)" % (off, ret)
        if len(sargs) > 8:
            return "lcn(%s, %s, %d, (const uint32_t[]){%s})" % (site, fname, len(sargs), ", ".join(sargs))
        return "lc%d(%s, %s%s)" % (len(sargs), site, fname, "".join(", " + a for a in sargs))

    def take_args(self, st, x, nargs, live):
        """the call's arguments: the pending pushes from esp up (or what the stack holds there); every
        other pending write is made first"""
        sp = st.esp
        args = []
        for i in range(nargs):
            s = sp + 4 * i
            if s in st.pend:
                args.append(st.pend.pop(s))
            else:
                # already written (by an earlier call's arguments, a store): read it back
                if st.bottom is not None and s >= st.bottom:
                    self.refuse(x, "argument slot inside the frame")
                args.append(X_("rd32(c, %s)" % FP(s + 4).c, reads={("f", s + 4, 4)}, atom=True))
                self.fp_used.add(s + 4)
        # the other pending writes happen before the call
        rest = sorted(set(st.pend) | set(st.dead))
        if rest:
            # careful: the argument values are evaluated at the call, after these writes
            for a in args:
                if a.lazy and any(self.slot_conflict(a, s) for s in rest):
                    self.refuse(x, "argument read overlaps a pending write")
            self.flush_held(st, x, live=live)
            self.write_slots(st, x, rest)
        for a in args:
            if is_undef(a):
                self.refuse(x, "undefined argument")
        return args

    def role(self, off, name, prio):
        cur = self.roles.get(off)
        if cur is None or cur[1] < prio:
            self.roles[off] = (name, prio)

    def note_roles(self, t, args):
        rn = RUNTIME.get(t, (None,))[0]
        if rn == "enter" and len(args) == 6:
            for a, nm in zip(args[1:], ("frame", "slots", "scope", "seen", "jb")):
                if a.kind == "fp":
                    self.role(a.n, nm, 9)
        if rn == "var_init" and len(args) == 4 and args[1].kind == "fp" and args[3].kind == "k":
            tp = args[3].n
            nm = TYPE_VAR.get(tp) or (STREAMS[tp] if tp < len(STREAMS) else "val")
            self.role(args[1].n, nm, 8)
        if rn == "var_init_sync" and len(args) == 2 and args[1].kind == "fp":
            self.role(args[1].n, "sync", 8)
        for a in args:
            if a.kind == "fp":
                self.role(a.n, "loc", 1)

    def arg_text(self, t, i, a):
        rn = RUNTIME.get(t, (None,))[0]
        if a.kind == "k":
            if rn == "var_init" and i == 3 and a.n in TYPE_NAMES:
                return TYPE_NAMES[a.n]
            n = a.n
            if n >= 0xffff0000:
                return "-%d" % ((-n) & M32)
            return str(n) if n < 0x10000 else "0x%x" % n
        return a.c

    # ---------------------------------------------------------------- blocks

    def run_block(self, bstart, st, emit):
        """execute a block; returns [(successor, state, edge kind, cond)] and, with emit, the lines"""
        self.out = [] if emit else None
        fn = self.fn
        insns = self.blocks[bstart]
        exits = []
        for a in insns[:-1]:
            self.step(st, fn.ins[a], self.after[a])
        x = fn.ins[insns[-1]]
        live = self.after[x.address]
        mn = x.mnemonic
        nxt = x.address + x.size
        term = mn.startswith("j") or mn in ("ret", "retn")
        if not term:
            self.step(st, x, live)
            self.end_block(st, x, live)
            exits.append((nxt, st, "fall", None))
            return exits
        if mn in ("ret", "retn"):
            if x.operands:
                self.refuse(x, "ret n")
            if st.esp != 0:
                self.refuse(x, "esp off by %d at ret" % st.esp)
            for r in ("ebx", "esi", "edi", "ebp"):
                v = st.r[r]
                if v.kind != "in" or v.n != r:
                    self.refuse(x, "%s not restored" % r)
            v = st.r["eax"]
            if "@H@" in v.hv:
                v = self.flush_held(st, x, force=True)
            self.end_block(st, x, {"eax"})
            if is_undef(v):
                self.refuse(x, "undefined return value")
            self.full(v, x)
            for h in v.hv:
                if h.startswith("@J"):
                    self.returns_join.add(h)
            v = self.unmachine(st, x, v)
            self.machine_regs(st, x, ALL_REGS)
            self.emit("LIFT_RETURN(%s);" % v.c)
            return exits
        if mn == "jmp":
            self.end_block(st, x, live)
            if x.address in fn.tables:
                idx = self.reg_get(st, x.reg_name(x.operands[0].mem.index), x)
                if idx.lazy:
                    # evaluated once, before the edges' copies
                    idx = self.temp(st, idx, x)
                tab = fn.tables[x.address]
                exits.append(("switch", idx, tab))
                for t in dict.fromkeys(tab):
                    exits.append((t, st, "case", None))
                return exits
            t = x.operands[0].imm & M32
            exits.append((t, st, "goto", None))
            return exits
        # a conditional jump
        cc = mn[1:]
        t = x.operands[0].imm & M32
        c_ = self.cond(st, cc, x)
        held_in_cond = st.held is not None and st.fl is not None and \
            any(isinstance(p, V) and "@H@" in p.hv for p in st.fl)
        if held_in_cond:
            eax_live = any("@H@" in v.hv and r in (live | self.live_in_of(t)) for r, v in st.r.items())
            if eax_live:
                self.flush_held(st, x, force=True)
                c_ = self.cond(st, cc, x)
            else:
                c_ = c_.replace("@H@", st.held)
                st.held = None
                self.replace_everywhere(st, "@H@", UNDEF)
        self.end_block(st, x, live, cond_parts=[p for p in (st.fl or ()) if isinstance(p, V)])
        c_ = self.simplify_cond(c_)
        exits.append((t, st, "taken", c_))
        exits.append((nxt, st, "fall", None))
        return exits

    def live_in_of(self, b):
        return self.live_in.get(b, set())

    def end_block(self, st, x, live, cond_parts=()):
        """the pending writes and the held call are made before the block's jump"""
        if st.pend or st.dead:
            self.flush_held(st, x, live=live)
            for s in sorted(set(st.pend) | set(st.dead)):
                v = st.pend.get(s, st.dead.get(s))
                for p in cond_parts:
                    if p.lazy and self.slot_conflict(p, s):
                        self.refuse(x, "condition reads a slot written at the block end")
            self.write_slots(st, x, sorted(set(st.pend) | set(st.dead)))
        self.flush_held(st, x, live=live)

    # ---------------------------------------------------------------- joins

    def merge(self, old, new, b, x):
        live = self.live_in[b]
        if new.pend or new.dead or new.held:
            raise Refuse("pending writes at a join (%08x)" % b)
        if old is not None and old.esp != new.esp:
            raise Refuse("esp differs at the join %08x (%d, %d)" % (b, old.esp, new.esp))
        s = St()
        s.esp = new.esp
        s.bottom = new.bottom if old is None else (old.bottom if new.bottom is None else
                                                    new.bottom if old.bottom is None else min(old.bottom, new.bottom))
        s.saved = dict(new.saved) if old is None else {k: v for k, v in new.saved.items() if old.saved.get(k) == v}
        s.mach = dict(new.mach) if old is None else {k: v for k, v in new.mach.items() if old.mach.get(k) == v}
        for r in REG32:
            nv = new.r.get(r, UNDEF)
            if r == "ebp":
                if old is not None and old.r["ebp"] != nv:
                    raise Refuse("ebp differs at the join %08x" % b)
                s.r[r] = nv
                continue
            if r not in live:
                s.r[r] = UNDEF
                continue
            jdb = min(nv.db, old.r[r].db if old is not None else 32)
            j = V("x", "@J%s@" % r, hv={"@J%s@" % r}, atom=True, db=jdb)
            keep = lambda v: v.pure or v == j
            if old is None:
                s.r[r] = nv if keep(nv) else j
            else:
                ov = old.r[r]
                if ov.c == j.c:
                    s.r[r] = j
                elif ov == nv and keep(nv):
                    s.r[r] = nv
                else:
                    s.r[r] = j
        if "fl" in live:
            if new.fl is None or (old is not None and (old.fl is None or fl_key(old.fl) != fl_key(new.fl))):
                raise Refuse("flags live at the join %08x" % b)
            if any(isinstance(p, V) and (p.hv or p.lazy) for p in new.fl):
                raise Refuse("flags with variables at the join %08x" % b)
            s.fl = new.fl
        return s

    def edge_copies(self, st, t, x):
        """the assignments on an edge into t: its join variables"""
        ent = self.entry[t]
        if self.npred[t] <= 1:
            return []
        pairs = []
        for r in REG32:
            ev = ent.r.get(r, UNDEF)
            if ev.kind == "x" and ev.c == "@J%s@" % r:
                v = st.r.get(r, UNDEF)
                if v.c == ev.c:
                    continue
                if v.kind == "u" and r in ("ecx", "edx"):
                    # a scratch register left undefined by a call on this path: the compiler only stores it
                    # as stack filler (push ecx before an x87 store), so it is what the machine's register holds
                    v = X_("c->%s" % r, reads={("r",)}, atom=True)
                elif is_undef(v):
                    self.refuse(x, "undefined %s into the join %08x" % (r, t))
                pairs.append((ev.c, v))
            elif ev.kind != "u":
                if st.r.get(r) != ev:
                    self.refuse(x, "%s differs into %08x" % (r, t))
        # sequentialize the parallel copy
        lines = []
        pairs = list(pairs)
        while pairs:
            for i, (d, v) in enumerate(pairs):
                if not any(d in w.hv for dd, w in pairs if dd != d):
                    lines.append("%s = %s;" % (d, v.c))
                    pairs.pop(i)
                    break
            else:
                d, v = pairs[0]
                tmp = "@T%x_%d@" % (x.address, self.tmpk)
                self.tmpk += 1
                lines.append("%s = %s;" % (tmp, d))
                tv = V("x", tmp, hv={tmp}, atom=True)
                pairs = [(dd, subst(w, d, tv)) for dd, w in pairs]
        return lines

    def expand(self, exits):
        """the exits with the jumps into flag-join blocks made in place: [(target, state, kind, cond, sub)]
        where sub (for a flag join) is [(taken target, cond), (fall target, None)]"""
        out = []
        for ex in exits:
            if ex[0] == "switch":
                out.append(ex)
                continue
            t, st, kind, c_ = ex
            if t in self.fj:
                if kind == "case":
                    raise Refuse("jump table into a flag join")
                xb, tb, fb = self.fj[t]
                cb = self.simplify_cond(self.cond(st, xb.mnemonic[1:], xb))
                out.append((t, st, kind, c_, [(tb, cb), (fb, None)]))
            else:
                out.append((t, st, kind, c_, None))
        return out

    def edges(self, exits):
        for ex in self.expand(exits):
            if ex[0] == "switch":
                continue
            t, st, kind, c_, sub = ex
            if sub:
                for tt, _ in sub:
                    yield tt, st
            else:
                yield t, st

    # ---------------------------------------------------------------- the function

    def run(self):
        fn = self.fn
        L = self.L
        e = fn.entry
        x0 = fn.ins[e]
        x1 = fn.ins.get(e + x0.size)
        if x0.mnemonic != "push" or x0.op_str != "ebp" or x1 is None or x1.mnemonic != "mov" or x1.op_str != "ebp, esp":
            raise Refuse("no frame prologue (a function starting mid-code?)")
        self.blocks = L.blocks(fn)
        succ = {b: L.succs(fn, self.blocks[b]) for b in self.blocks}
        for b, ss in succ.items():
            for s_ in ss:
                if s_ not in self.blocks:
                    raise Refuse("jump into the middle of a block %08x" % s_)
        self.succ = succ
        self.live_in, self.after = L.liveness(fn, self.blocks, succ)
        preds = collections.defaultdict(set)
        for b, ss in succ.items():
            for s_ in ss:
                preds[s_].add(b)
        self.fj = {}
        for b, ins in self.blocks.items():
            x = fn.ins[ins[0]]
            if len(ins) == 1 and x.mnemonic.startswith("j") and x.mnemonic != "jmp" and len(preds[b]) >= 2 \
                    and b != e:
                self.fj[b] = (x, succ[b][0], succ[b][1])
        for b, (x, tb, fb) in self.fj.items():
            if tb in self.fj or fb in self.fj:
                raise Refuse("chained flag joins at %08x" % b)
        succ = {b: [t for s_ in ss for t in ((self.fj[s_][1], self.fj[s_][2]) if s_ in self.fj else (s_,))]
                for b, ss in succ.items()}
        self.succ2 = succ
        preds = collections.defaultdict(set)
        for b, ss in succ.items():
            for s_ in ss:
                preds[s_].add(b)
        self.npred = {b: len(preds[b]) + (1 if b == e else 0) for b in self.blocks}
        # reverse post order
        order, seen = [], set()

        def dfs(b):
            stack = [(b, iter(succ[b]))]
            seen.add(b)
            while stack:
                n, it = stack[-1]
                for s_ in it:
                    if s_ not in seen:
                        seen.add(s_)
                        stack.append((s_, iter(succ[s_])))
                        break
                else:
                    order.append(n)
                    stack.pop()
        dfs(e)
        rpo = list(reversed(order))
        init = St()
        for r in ("eax", "ecx", "edx"):
            init.r[r] = X_("c->" + r, reads={("r",)}, atom=True)
        for r in ("ebx", "esi", "edi", "ebp"):
            init.r[r] = V("in", "in_" + r, r, atom=True)
            if r != "ebp":
                init.mach[r] = "in_" + r
        self.entry = {e: init}
        # fixpoint over the join states
        for it in range(50):
            changed = False
            for b in rpo:
                if b not in self.entry:
                    continue
                self.tmpk = 0
                exits = self.run_block(b, self.entry[b].copy(), False)
                for t, st in self.edges(exits):
                    if self.npred[t] <= 1:
                        new = st.copy()
                        new.held = None
                    else:
                        new = self.merge(self.entry.get(t), st, t, fn.ins[self.blocks[b][-1]])
                    if t not in self.entry or new.key() != self.entry[t].key():
                        self.entry[t] = new
                        changed = True
            if not changed:
                break
        else:
            raise Refuse("no fixpoint")
        # the setjmp must not be inside a loop
        # emission, in address order
        texts = {}
        self.tmpk = 0
        for b in rpo:
            self.tmpk = 0
            st = self.entry[b].copy()
            exits = self.run_block(b, st, True)
            lines = self.out
            x = fn.ins[self.blocks[b][-1]]
            switch = None
            for ex in exits:
                if ex[0] == "switch":
                    switch = ex
                    continue
            if switch:
                _, idx, tab = switch
                lines.append("switch (%s) {" % idx.c)
                for i, t in enumerate(tab):
                    cp = self.edge_copies(exits[1][1], t, x)
                    lines.append("case %d: %sgoto @L%x@;" % (i, "".join(l + " " for l in cp), t))
                lines.append('default: x86_fail(c, 0x%xu, "jump table index");' % x.address)
                lines.append("}")
            else:
                for t, st2, kind, c_, sub in self.expand(exits):
                    if sub:
                        (tb, cb), (fb, _) = sub
                        inner = []
                        cp = self.edge_copies(st2, tb, x)
                        if cp:
                            inner += ["if (%s) {" % cb] + ["    " + l for l in cp] + ["    goto @L%x@;" % tb, "}"]
                        else:
                            inner.append("if (%s) goto @L%x@;" % (cb, tb))
                        inner += self.edge_copies(st2, fb, x)
                        if kind == "taken":
                            lines.append("if (%s) {" % c_)
                            lines.extend("    " + l for l in inner)
                            lines.append("    goto @L%x@;" % fb)
                            lines.append("}")
                        else:
                            lines.extend(inner)
                            lines.append("@GOTO %x@" % fb)
                        continue
                    cp = self.edge_copies(st2, t, x)
                    if kind == "taken":
                        if cp:
                            lines.append("if (%s) {" % c_)
                            lines.extend("    " + l for l in cp)
                            lines.append("    goto @L%x@;" % t)
                            lines.append("}")
                        else:
                            lines.append("if (%s) goto @L%x@;" % (c_, t))
                    else:
                        lines.extend(cp)
                        lines.append("@GOTO %x@" % t)
            texts[b] = lines
        # one setjmp, and its block must not be reachable from itself
        sj = [b for b, ls in texts.items() for l in ls if "GUEST_SETJMP" in l]
        if len(sj) != 1:
            raise Refuse("setjmp count %d" % len(sj))
        succ = self.succ2
        reach, todo = set(), list(succ[sj[0]])
        while todo:
            n = todo.pop()
            if n in reach:
                continue
            reach.add(n)
            todo.extend(succ[n])
        if sj[0] in reach:
            raise Refuse("setjmp inside a loop")
        return self.render(texts)

    # ---------------------------------------------------------------- output

    def render(self, texts):
        fn = self.fn
        e = fn.entry
        addrs = [b for b in self.blocks if b in texts]
        addrs.sort()
        used_labels = set()
        body = []
        for i, b in enumerate(addrs):
            nxt = addrs[i + 1] if i + 1 < len(addrs) else None
            for l in texts[b]:
                m = re.match(r"^@GOTO ([0-9a-f]+)@$", l)
                if m:
                    t = int(m.group(1), 16)
                    if t != nxt:
                        body.append((b, "goto @L%x@;" % t))
                        used_labels.add(t)
                    continue
                for m in re.finditer(r"@L([0-9a-f]+)@", l):
                    used_labels.add(int(m.group(1), 16))
                body.append((b, l))
        # names
        text = "\n".join("%x\t%s" % (b, l) for b, l in body)
        tnames = {}
        for m in re.finditer(r"@T[0-9a-f]+_\d+@", text):
            if m.group(0) not in tnames:
                tnames[m.group(0)] = "t%d" % (len(tnames) + 1)
        jnames = {}
        for m in re.finditer(r"@J(\w+)@", text):
            if m.group(0) not in jnames:
                r = m.group(1)
                jnames[m.group(0)] = "result" if m.group(0) in self.returns_join and r != "eax" else \
                    {"eax": "x", "ecx": "y", "edx": "z", "ebx": "b", "esi": "s", "edi": "d"}[r]
        if "result" in jnames.values() and list(jnames.values()).count("result") > 1:
            for k in jnames:
                if jnames[k] == "result":
                    jnames[k] = "result_" + k[2:-1]
        # frame names
        fnames = {}
        used = set()
        SIZE = {"frame": 0x44, "jb": 0x40}
        extent = {}
        for off in sorted(self.roles):
            nm = self.roles[off][0]
            if off >= 0:
                continue
            base = "%s_%x" % (nm, -off)
            if nm in ("frame", "slots", "scope", "seen", "jb") and nm not in used:
                base = nm
            used.add(nm)
            fnames[off] = base
            extent[off] = SIZE.get(nm, VALUE_SIZE.get(nm, 8 if nm in STREAMS or nm == "val" else 4))
        # the other locals the code touches (not the saved registers)
        saved_slots = set()
        for off in sorted(self.fp_used):
            if off in fnames or off >= 0 or self.bottom is not None and off - 4 < self.bottom:
                continue
            inside = [o for o in fnames if o < off < o + extent[o]]
            if inside:
                continue
            if off in self.saved_offs:
                continue
            fnames[off] = "loc_%x" % -off
            extent[off] = 4
        pnames = {0: "eng"}

        def fpname(off):
            if off == 0:
                return "fp"
            if off in fnames:
                return fnames[off]
            inside = [o for o in fnames if o < off < o + extent[o]]
            if inside:
                o = max(inside)
                return "%s + %d" % (fnames[o], off - o)
            return "fp - 0x%x" % -off if off < 0 else "fp + 0x%x" % off

        decl_frame = sorted(fnames.items())

        def sub(l):
            l = re.sub(r"@T[0-9a-f]+_\d+@", lambda m: tnames[m.group(0)], l)
            l = re.sub(r"@J\w+@", lambda m: jnames[m.group(0)], l)
            l = re.sub(r"(?<=[(, ])@F(-?\d+)@(?=[,)])", lambda m: fpname(int(m.group(1))), l)
            l = re.sub(r"@F(-?\d+)@", lambda m: fpname(int(m.group(1))) if " " not in fpname(int(m.group(1)))
                       else "(%s)" % fpname(int(m.group(1))), l)
            l = re.sub(r"@P(\d+)@", lambda m: pnames.get(int(m.group(1)), "p%d" % int(m.group(1))), l)
            l = re.sub(r"@L([0-9a-f]+)@", lambda m: "L_%s" % m.group(1), l)
            if "@" in l:
                raise Refuse("unresolved marker in %r" % l)
            return l

        out = ["/* FUN_%08x - a rule, lifted from its machine code by tools/delta_lift.py */" % e,
               "LIFTED_FN(%08x)" % e, "{", "    const uint32_t fp = c->esp - 4;",
               "    const uint32_t in_ebx = c->ebx, in_esi = c->esi, in_edi = c->edi, in_ebp = c->ebp;"]
        if self.params:
            out.append("    const uint32_t %s;" % ", ".join(
                "%s = rd32(c, fp + 0x%x)" % (pnames.get(i, "p%d" % i), 8 + 4 * i) for i in sorted(self.params)))
        if decl_frame:
            items = ["%s = fp - 0x%x" % (n, -o) for o, n in decl_frame if o < 0]
            for i in range(0, len(items), 5):
                out.append("    const uint32_t %s;" % ", ".join(items[i:i + 5]))
        if tnames:
            items = list(tnames.values())
            for i in range(0, len(items), 16):
                out.append("    uint32_t %s;" % ", ".join(items[i:i + 16]))
        if jnames:
            out.append("    uint32_t %s;" % ", ".join(sorted(set(jnames.values()))))
        by_block = collections.OrderedDict((b, []) for b in addrs)
        for b, l in body:
            by_block[b].append(l)
        for b, ls in by_block.items():
            if b in used_labels:
                out.append("L_%x:" % b)
            out.extend("    " + sub(l) for l in ls)
        out.append("}")
        # labels at the end of a block list need a statement
        res = []
        for i, l in enumerate(out):
            res.append(l)
            if re.match(r"^L_[0-9a-f]+:$", l) and (i + 1 >= len(out) or out[i + 1] == "}"):
                res.append("    ;")
        return "\n".join(res) + "\n" + "".join("/*import %s*/\n" % i for i in sorted(self.imports))


# ------------------------------------------------------------------------------------------------ driver

def rule_candidates(L, entries):
    out = []
    for e in entries:
        try:
            fn = L.function(e)
        except Refuse:
            continue
        if L.is_rule(fn):
            out.append(e)
    return out


def write_if_changed(path, text):
    if os.path.exists(path) and open(path, newline="\n").read() == text:
        return False
    open(path, "w", newline="\n").write(text)
    return True


def runtime_header():
    lines = ["/* lift_rt.h - generated by tools/delta_lift.py: the rule runtime's entry points by name, for the",
             " * lifted rules. r_<name>(S(off, ret), args) writes the arguments where the original pushes them and",
             " * calls the hand port (src/rules*.c, adapters in rules_guest.c) at the original's esp. */",
             "#ifndef LIFT_RT_H", "#define LIFT_RT_H", '#include "lift.h"', ""]
    for a in sorted(RUNTIME, key=lambda k: RUNTIME[k][0]):
        lines.append("void f_%08x(cpu *c);" % a)
    lines.append("")
    for a in sorted(RUNTIME, key=lambda k: RUNTIME[k][0]):
        name, n = RUNTIME[a]
        ps = "".join(", uint32_t a%d" % i for i in range(n))
        as_ = "".join(", a%d" % i for i in range(n))
        lines.append("static inline uint32_t r_%s(cpu *c, uint32_t sp, uint32_t ret%s) { return lc%d(c, sp, ret, f_%08x%s); }"
                     % (name, ps, n, a, as_))
    lines += ["", "enum { T_SYM8 = -1, T_SYM16 = -2, T_INT = -3, T_SHORT = -4, T_DOUBLE = -5, T_SYNC = -6 };", "", "#endif", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=os.path.join(ROOT, "pkg", "ENU.SYN"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "src", "gen"))
    ap.add_argument("--only", default="", help="comma-separated addresses")
    ap.add_argument("--exclude", default="", help="comma-separated addresses not to lift")
    ap.add_argument("--max", type=int, default=0, help="lift at most this many (smallest first)")
    ap.add_argument("--split", type=int, default=16)
    ap.add_argument("--entries", default=os.path.join(ROOT, "build", "all_entries.txt"))
    ap.add_argument("--print", dest="show", default="", help="print the lifted C of this function")
    ap.add_argument("--dry", action="store_true", help="report only, write nothing")
    a = ap.parse_args()
    L = Lifter(a.image)
    entries = [int(x, 16) for x in open(a.entries).read().split()]
    L.scan_calls(entries)
    if a.show:
        try:
            print(L.lift(int(a.show, 16)))
        except Refuse as e:
            print("refused:", e)
        return
    ported = set(L.ported)
    cands = [e for e in rule_candidates(L, entries) if e not in ported]
    if a.only:
        only = {int(x, 16) for x in a.only.split(",") if x}
        cands = [e for e in cands if e in only]
    excl = {int(x, 16) for x in a.exclude.split(",") if x}
    lifted, refused = {}, {}
    for e in cands:
        if e in excl:
            refused[e] = "excluded"
            continue
        try:
            lifted[e] = L.lift(e)
        except Refuse as ex:
            refused[e] = str(ex)
    if a.max and len(lifted) > a.max:
        keep = sorted(lifted, key=lambda k: len(lifted[k]))[:a.max]
        for k in list(lifted):
            if k not in keep:
                refused[k] = "over --max"
                del lifted[k]
    reasons = collections.Counter(re.sub(r" at [0-9a-f]{8}.*| [0-9a-f]{8}|\(.*", "", r).strip() for r in refused.values())
    print("delta_lift: %d rule functions, %d lifted, %d refused" % (len(cands), len(lifted), len(refused)))
    for r, n in reasons.most_common():
        print("  %5d  %s" % (n, r))
    if a.dry:
        return
    os.makedirs(a.out, exist_ok=True)
    names = sorted(lifted)
    hand_h = os.path.join(ROOT, "src", "rules", "hand.h")
    hand = set()
    if os.path.exists(hand_h):
        hand = {int(m, 16) for m in re.findall(r"^HAND_RULE\(([0-9a-f]{8})\)", open(hand_h).read(), re.M)}
    missing = hand - set(names)
    if missing:
        print("delta_lift: hand-written rules that are not lifted rules: %s" % " ".join("%08x" % m for m in sorted(missing)))
    print("delta_lift: %d of them written by hand (src/rules/hand.h), not emitted" % len(hand & set(names)))
    per = max(1, (len(names) + a.split - 1) // a.split)
    head = ["/* generated by tools/delta_lift.py from ENU.SYN's machine code: the Delta rules, lifted to C. */",
            '#include "lift_rt.h"', "", "#if defined(_MSC_VER)", "#pragma warning(disable: 4102 4146 4244)", "#endif", ""]
    decls = ["void f_%08x(cpu *c);" % t for t in sorted({int(m, 16) for s in lifted.values()
                                                        for m in re.findall(r"f_([0-9a-f]{8})\b", s)})]
    decls += ["void %s(cpu *c);" % i for i in sorted({m for s in lifted.values()
                                                      for m in re.findall(r"/\*import (\w+)\*/", s)})]
    old = set(os.path.basename(p) for p in os.listdir(a.out) if re.match(r"rules_lifted_\d+\.c$", p))
    written = set()
    for k in range(a.split):
        part = names[k * per:(k + 1) * per]
        if not part and k:
            continue
        fname = "rules_lifted_%02d.c" % k
        written.add(fname)
        write_if_changed(os.path.join(a.out, fname),
                         "\n".join(head + decls + [""] + [lifted[e] for e in part if e not in hand]) + "\n")
    for f in old - written:
        os.remove(os.path.join(a.out, f))
    write_if_changed(os.path.join(a.out, "lifted.h"),
                     "/* generated by tools/delta_lift.py: the lifted rules (hand ports in the sense of src/ported.h) */\n" +
                     "".join("PORTED(%08x, 0)\n" % e for e in names))
    write_if_changed(os.path.join(a.out, "lift_rt.h"), runtime_header())
    with open(os.path.join(a.out, "lift_report.txt"), "w", newline="\n") as f:
        f.write("lifted %d refused %d\n" % (len(lifted), len(refused)))
        for e in sorted(refused):
            f.write("%08x %s\n" % (e, refused[e]))


if __name__ == "__main__":
    main()
