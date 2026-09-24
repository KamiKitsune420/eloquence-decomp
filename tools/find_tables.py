"""Look for openevv's synthesizer tables (IBM Embedded ViaVoice 4.3) inside a binary, byte for byte.

  python tools/find_tables.py <openevv/src/klatt_tables.c> <binary>

Each C array in the table file is packed with its declared element type and searched for in the binary;
also a 64-byte prefix, in case only the start matches.
"""
import re
import struct
import sys

FMT = {"int16_t": "<h", "uint16_t": "<H", "int32_t": "<i", "uint32_t": "<I", "int8_t": "<b", "uint8_t": "<B"}


def arrays(src):
    for m in re.finditer(r"const\s+(\w+)\s+(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\};", src, re.S):
        typ, name, body = m.groups()
        if typ not in FMT:
            continue
        vals = [int(v, 0) for v in re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", re.sub(r"/\*.*?\*/", "", body, flags=re.S))]
        yield name, b"".join(struct.pack(FMT[typ], v) for v in vals), len(vals)


def main():
    src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    blob = open(sys.argv[2], "rb").read()
    for name, data, n in arrays(src):
        at = blob.find(data)
        pre = blob.find(data[:64])
        print("%-22s %5d elems  full: %-10s prefix64: %s" % (
            name, n, hex(at) if at >= 0 else "-", hex(pre) if pre >= 0 else "-"))


main()
