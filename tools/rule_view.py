r"""rule_view - a compiled Delta rule, lifted, with its data spelled out: for writing it by hand.

  python tools/rule_view.py 100dbdd1 [more addresses]

Prints the lifted C of each rule (tools/delta_lift.py) followed by what its constants are: the stream
lists it scopes to, the symbol strings it matches, the symbol lists it inserts - each decoded with the
streams' own symbol names from ENU.SYN (stream table at 0x10150be8, see src/rules.h). Needs pkg/ENU.SYN.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import delta_lift  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STREAM_TABLE, STREAM_SIZE, FIELD_SIZE = 0x10150be8, 0x39, 0x15


class Data:
    def __init__(self, img):
        self.img = img

    def b(self, a, n):
        o = self.img.off(a)
        return self.img.d[o:o + n]

    def u32(self, a):
        return struct.unpack("<I", self.b(a, 4))[0]

    def s16(self, a):
        return struct.unpack("<h", self.b(a, 2))[0]

    def cstr(self, a):
        o = self.img.off(a)
        return self.img.d[o:self.img.d.index(b"\0", o)].decode("latin1")

    def stream_name(self, s):
        return self.cstr(self.u32(STREAM_TABLE + STREAM_SIZE * s)) if 0 <= s < 9 else "stream%d" % s

    def symbol(self, s, code, field=0):
        """the name of symbol `code` of the stream's field"""
        if not 0 <= s < 9:
            return str(code)
        fd = self.u32(STREAM_TABLE + STREAM_SIZE * s + 4) + FIELD_SIZE * field
        n = self.s16(fd + 0x10)
        if not 0 <= code < n:
            return "?%d" % code
        return self.cstr(self.u32(self.u32(fd + 8) + 4 * code))


def num(t):
    t = t.strip().rstrip("u")
    return int(t, 0)


def explain(d, text):
    notes = []
    for m in re.finditer(r"f_1004bdb2, eng, (\w+), (0x[0-9a-f]+)", text):
        n, p = num(m.group(1)), num(m.group(2))
        notes.append("scope %s: %s" % (m.group(2), ", ".join(d.stream_name(x) for x in d.b(p, n))))
    for m in re.finditer(r"r_match_string\(S\([^)]*\), eng, (\w+), (\w+), (0x[0-9a-f]+)\)", text):
        s, n, p = num(m.group(1)), num(m.group(2)), num(m.group(3))
        notes.append("match %s in %s: \"%s\"" % (m.group(3), d.stream_name(s),
                                                  " ".join(d.symbol(s, x) for x in d.b(p, n))))
    for m in re.finditer(r"r_match_shorts\(S\([^)]*\), eng, (\w+), (\w+), (0x[0-9a-f]+)\)", text):
        s, n, p = num(m.group(1)), num(m.group(2)), num(m.group(3))
        notes.append("match %s in %s (shorts): %s" % (m.group(3), d.stream_name(s),
                                                       " ".join(d.symbol(s, d.s16(p + 2 * i)) for i in range(n))))
    for m in re.finditer(r"r_insert_list(_shorts)?\(S\([^)]*\), eng, (\w+), (\w+), ([^,]+), ", text):
        s, n = num(m.group(2)), num(m.group(3))
        notes.append("insert into %s, %d symbol(s) from the list argument" % (d.stream_name(s), n))
    lists = set()
    for m in re.finditer(r"r_insert_list(_shorts)?\(S\([^)]*\), eng, (\w+), (\w+), rd32\(c, fp - (0x[0-9a-f]+)\)", text):
        lists.add((num(m.group(2)), num(m.group(3)), m.group(4)))
    for s, n, slot in sorted(lists):
        for v in sorted(set(re.findall(r"wr32\(c, fp - %s, (0x[0-9a-f]+)u\)" % slot, text))):
            p = num(v)
            notes.append("  list %s: %s" % (v, " ".join(d.symbol(s, d.u32(p + 4 * i)) for i in range(n))))
    return notes


def main():
    L = delta_lift.Lifter(os.path.join(ROOT, "pkg", "ENU.SYN"))
    entries = [int(x, 16) for x in open(os.path.join(ROOT, "build", "all_entries.txt")).read().split()]
    L.scan_calls(entries)
    d = Data(L.img)
    for a in sys.argv[1:]:
        text = L.lift(int(a, 16))
        print(text)
        for n in explain(d, text):
            print("// " + n)
        print()


main()
