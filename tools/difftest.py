r"""Run the differential tester (build\<arch>\difftest.exe, src/difftest.c) over the whole test matrix and
sum its reports: every hand-ported function checked against the recompiled original at every call.

  python tools/difftest.py [--arch x64|x86] [--only 10131790,101311a0] [--quick] [-j 8]

The matrix: tests/corpus.txt, hard.txt, escapes.txt line by line in the 8 voice presets; tests/long.txt
line by line in the 8 presets; annotated texts (ELOQ_ANNOT) in 4 presets; 6 lines at 8000, 22050 and
48000 Hz (ELOQ_RATE); 3 lines through each of 4 voice effects (ELOQ_FX); corpus and hard as one long text
each in 2 presets. --quick: voice 1 only, no long texts.

Also checks that the audio of each case is identical to eloq_run's (build\<arch>\eloq_run.exe), since the
tester's own run continues with the hand ports' results.

Prints per function: calls, checked, mismatches and the first mismatch; exit status 1 on any mismatch.
"""
import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lines(name):
    return [l.rstrip("\r\n") for l in open(os.path.join(ROOT, "tests", name), encoding="utf-8", errors="replace")
            if l.strip()]


ANNOTATED = [
    "`vs90 This is fast speech. `vs10 And this is slow speech. `vb90 high pitch `vb10 low pitch `vf100 lots of "
    "fluctuation `vf0 none.",
    "`vs100 `vr50 `vy80 rough and breathy `vh90 big head `vg1 now female `vv50 quiet `vv100 loud.",
    "`vs0 Very very slow speech with pauses. After a pause, more words.",
    "`vb100 `vs100 Maximum pitch and speed for the numbers 123456789.",
    "`pp0 `vs70 Hello `vb30 there `vf80 my `vs20 friend, `vs95 how `vb0 are `vr100 you?",
    "`vs50 `vb50 `vh0 `vr0 `vy0 plain voice, then `vh100 `vr100 `vy100 everything.",
]


def cases(quick):
    out = []
    voices = (1,) if quick else range(1, 9)
    allt = [(n, i, l) for n in ("corpus.txt", "hard.txt", "escapes.txt") for i, l in enumerate(lines(n))]
    if not quick:
        allt += [("long.txt", i, l) for i, l in enumerate(lines("long.txt"))]
    for v in voices:
        for n, i, l in allt:
            out.append(("%s%d_v%d" % (n[:4], i, v), l, {"ELOQ_VOICE": str(v)}))
    for v in ((1,) if quick else (1, 2, 5, 8)):
        for i, t in enumerate(ANNOTATED):
            out.append(("ann%d_v%d" % (i, v), t, {"ELOQ_VOICE": str(v), "ELOQ_ANNOT": "1"}))
    some = lines("corpus.txt")[:4] + lines("hard.txt")[:2]
    for r in ("8000", "22050", "48000"):
        for v in ((1,) if quick else (1, 3)):
            for i, t in enumerate(some):
                out.append(("rate%s_%d_v%d" % (r, i, v), t, {"ELOQ_VOICE": str(v), "ELOQ_RATE": r}))
    for k, fx in enumerate(("whisper", "pitch=1.5,vib=50:5", "mono=100,oq=70,formant=1.2,bw=1.3",
                            "jitter=30,breath=6")):
        for i, t in enumerate(some[:3]):
            out.append(("fx%d_%d" % (k, i), t, {"ELOQ_FX": fx}))
    if not quick:
        for v in (1, 6):
            out.append(("longcorpus_v%d" % v, " ".join(lines("corpus.txt")), {"ELOQ_VOICE": str(v)}))
            out.append(("longhard_v%d" % v, " ".join(lines("hard.txt")), {"ELOQ_VOICE": str(v)}))
    return out


def env_for(extra, only):
    e = dict(os.environ)
    for k in ("ELOQ_VOICE", "ELOQ_ANNOT", "ELOQ_RATE", "ELOQ_FX", "ELOQ_FORMANT", "DIFFTEST", "DIFFTEST_VERBOSE"):
        e.pop(k, None)
    e.update(extra)
    if only:
        e["DIFFTEST"] = only
    e["DIFFTEST_VERBOSE"] = "0"
    return e


def wav_hash(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha1(open(path, "rb").read()).hexdigest()
    os.remove(path)
    return h


def run(arch, tmp, case, only):
    name, text, extra = case
    e = env_for(extra, only)
    wa, wb = os.path.join(tmp, name + "_dt.wav"), os.path.join(tmp, name + "_er.wav")
    r = subprocess.run([os.path.join(ROOT, "build", arch, "difftest.exe"), "-", text, wa], capture_output=True,
                       text=True, env=e)
    r2 = subprocess.run([os.path.join(ROOT, "build", arch, "eloq_run.exe"), "-", text, wb], capture_output=True,
                        text=True, env=e)
    fns = {}
    total = None
    for l in r.stderr.splitlines():
        m = re.match(r"DIFFTEST ([0-9a-f]{8}) calls (\d+) checked (\d+) bad (\d+)(?: first: (.*))?$", l)
        if m:
            fns[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5))
        m = re.match(r"DIFFTEST_RESULT calls (\d+) checked (\d+) bad (\d+)", l)
        if m:
            total = tuple(map(int, m.groups()))
    ha, hb = wav_hash(wa), wav_hash(wb)
    return name, r.returncode, fns, total, ha == hb and ha is not None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="x64")
    ap.add_argument("--only", default="")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args()
    cs = cases(a.quick)
    tmp = tempfile.mkdtemp(prefix="difftest_")
    agg = {}
    problems = []
    tot = [0, 0, 0]
    with cf.ThreadPoolExecutor(a.j) as ex:
        for name, rc, fns, total, same in ex.map(lambda c: run(a.arch, tmp, c, a.only), cs):
            if total is None or rc != 0:
                problems.append("%s: no result (exit %d)" % (name, rc))
                continue
            if not same:
                problems.append("%s: audio differs from eloq_run's" % name)
            for k in range(3):
                tot[k] += total[k]
            for f, (calls, checked, bad, first) in fns.items():
                g = agg.setdefault(f, [0, 0, 0, None])
                g[0] += calls
                g[1] += checked
                g[2] += bad
                if bad and g[3] is None:
                    g[3] = "%s: %s" % (name, first)
    for f in sorted(agg):
        calls, checked, bad, first = agg[f]
        print("%s calls %10d checked %10d bad %6d%s" % (f, calls, checked, bad, "  first: " + first if first else ""))
    print("%d cases: calls %d, checked %d, mismatches %d" % (len(cs), tot[0], tot[1], tot[2]))
    for p in problems[:40]:
        print(p)
    print("%d problem cases" % len(problems))
    sys.exit(1 if tot[2] or problems else 0)


main()
