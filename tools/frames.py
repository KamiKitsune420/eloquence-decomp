"""Read ecisay --dump-frames files (FRM2 records).

  python tools/frames.py build/hello.frm [--wav build/hello1.wav]

Each record: state address, ENU.SYN base, the 64-float frame, the synthesizer state before and after,
return value, the int32 samples it produced. --wav checks the concatenated samples against the WAV
ECI.DLL delivered.
"""
import struct
import sys

import numpy as np


def load(path):
    d = open(path, "rb").read()
    i = 0
    out = []
    while i < len(d):
        assert d[i:i + 4] == b"FRM2", "bad record at %d" % i
        addr, base = struct.unpack_from("<II", d, i + 4)
        frame = np.frombuffer(d, "<f4", 64, i + 12)
        i += 12 + 256
        sz = struct.unpack_from("<I", d, i)[0]
        before = d[i + 4:i + 4 + sz]
        after = d[i + 4 + sz:i + 4 + 2 * sz]
        i += 4 + 2 * sz
        ret, n = struct.unpack_from("<II", d, i)
        out_s = np.frombuffer(d, "<i4", n, i + 8)
        i += 8 + 4 * n
        out.append(dict(addr=addr, base=base, frame=frame, before=before, after=after, ret=ret, out=out_s))
    return out


def main():
    recs = load(sys.argv[1])
    ns = [len(r["out"]) for r in recs]
    allout = np.concatenate([r["out"] for r in recs]) if recs else np.zeros(0)
    print("%d frames, samples per frame %s, total %d, state at 0x%x, base 0x%x" % (
        len(recs), sorted(set(ns)), len(allout), recs[0]["addr"], recs[0]["base"]))
    print("int32 range %d..%d" % (allout.min(), allout.max()))
    if "--wav" in sys.argv:
        import wave
        w = wave.open(sys.argv[sys.argv.index("--wav") + 1])
        pcm = np.frombuffer(w.readframes(w.getnframes()), "<i2")
        n = min(len(pcm), len(allout))
        clip = np.clip(allout, -32768, 32767)
        print("wav %d samples; equal to clipped synth output over %d: %s; first diff %s" % (
            len(pcm), n, np.array_equal(pcm[:n], clip[:n]),
            np.argmax(pcm[:n] != clip[:n]) if not np.array_equal(pcm[:n], clip[:n]) else "-"))


if __name__ == "__main__":
    main()
