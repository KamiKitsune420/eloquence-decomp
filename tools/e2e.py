"""The recompiled engine (build/x64/eloq_run.exe) against real Eloquence (build/ecisay.exe), line by line.

  python tools/e2e.py tests/hard.txt [--limit N] [--voice 1-8]

Each line is spoken by both in a fresh process and the WAVs are compared sample for sample.
"""
import argparse
import os
import subprocess
import tempfile
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECISAY = os.path.join(ROOT, "build", "ecisay.exe")
PORT = os.path.join(ROOT, "build", "x64", "eloq_run.exe")
SYN = os.path.join(ROOT, "pkg", "ENU.SYN")


def samples(path):
    with wave.open(path) as w:
        return np.frombuffer(w.readframes(w.getnframes()), "<i2")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--voice", type=int, default=1, help="ECI.INI preset 1-8")
    ap.add_argument("--syn", default=SYN, help="ENU.SYN for the port, or - for the data compiled in")
    a = ap.parse_args()
    lines = [l.rstrip("\r\n") for l in open(a.text, encoding="utf-8", errors="replace") if l.strip()]
    if a.limit:
        lines = lines[:a.limit]
    tmp = tempfile.mkdtemp()
    ref, out = os.path.join(tmp, "ref.wav"), os.path.join(tmp, "port.wav")
    bad = 0
    for i, line in enumerate(lines):
        subprocess.run([ECISAY, "--voice", str(a.voice), line, ref], capture_output=True)
        r = subprocess.run([PORT, a.syn, line, out], capture_output=True, text=True,
                           env=dict(os.environ, ELOQ_VOICE=str(a.voice)))
        if r.returncode != 0:
            bad += 1
            print("line %d: port failed: %s" % (i, (r.stderr.strip().splitlines() or ["?"])[-1]))
            continue
        e, p = samples(ref), samples(out)
        if len(e) != len(p) or not np.array_equal(e, p):
            bad += 1
            n = min(len(e), len(p))
            first = int(np.argmax(e[:n] != p[:n])) if n and not np.array_equal(e[:n], p[:n]) else n
            print("line %d differs (%d vs %d samples, first at %d): %s" % (i, len(e), len(p), first, line[:60]))
    print("%d of %d lines identical" % (len(lines) - bad, len(lines)))


if __name__ == "__main__":
    main()
