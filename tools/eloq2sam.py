"""Eloquence's front end, Microsoft Sam's voice: Eloquence decides the phonemes, their timing and the
intonation; Sam's units and LPC vocoder (the tts-random port, sam_tts_phones) say them.

  python tools/eloq2sam.py "text" out.wav [--pitch-scale 1.0] [--keep]

Needs build/x64/eloq_phon.exe (timed phonemes + F0 frames) and build/x64/ECI.DLL (the transcription with
stress and words), and the Sam port's sam_say.exe with its voice data.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from eci_phonemes import phonemes  # noqa: E402

SAM = r"D:\llm-experiments\misk\tts-random"
SAM_SAY = os.path.join(SAM, "build", "x64", "sam_say.exe")
SAM_DATA = os.path.join(SAM, "data", "voice")

# Eloquence (US English) phoneme symbols -> SAPI 5 phone ids (Sam's lexicon uses these)
SAPI = dict(aa=10, ae=11, ah=12, ao=13, aw=14, ax=15, ay=16, b=17, ch=18, d=19, dh=20, eh=21, er=22, ey=23,
            f=24, g=25, h=26, ih=27, iy=28, jh=29, k=30, l=31, m=32, n=33, ng=34, ow=35, oy=36, p=37, r=38,
            s=39, sh=40, t=41, th=42, uh=43, uw=44, v=45, w=46, y=47, z=48, zh=49)
ELOQ = {"i": "iy", "I": "ih", "e": "ey", "E": "eh", "@": "ae", "a": "aa", "c": "ao", "o": "ow", "U": "uh",
        "u": "uw", "H": "ah", "x": "ax", "X": "ih", "A": "ae", "R": "er", "Y": "ay", "W": "aw", "O": "oy",
        "T": "th", "D": "dh", "S": "sh", "Z": "zh", "C": "ch", "J": "jh", "G": "ng", "N": "n", "L": "l",
        "F": "d", "?": "t"}
VOWELS = set("iIeE@AacoUuHxXRYWO")
# what the timed phoneme stream may show for a transcription symbol (diphthongs come out split)
SHOWS = {"W": "aw", "Y": "ay", "O": "cy", "J": "dZJ", "C": "tSC", "X": "Xx"}


def parse_transcription(tr):
    """words -> list of (symbols with stress) : [[('h',0),('E',2), '.', ('l',0),('o',1)], ...]"""
    words = []
    for m in re.finditer(r"\[([^\]]*)\]", tr):
        body, word, stress = m.group(1), [], 0
        i = 0
        while i < len(body):
            ch = body[i]
            if ch == ".":
                if i + 1 < len(body) and body[i + 1].isdigit():
                    stress = int(body[i + 1])
                    i += 2
                else:
                    i += 1
                if word:
                    word.append(".")
                continue
            word.append((ch, stress))
            i += 1
        words.append(word)
    return words


def read_timing(path):
    ph, frames, end = [], [], 0
    for line in open(path, encoding="latin-1"):
        f = line.split()
        if not f:
            continue
        if f[0] == "P":
            ph.append((int(f[1]), f[2] if len(f) > 2 else "?"))
        elif f[0] == "F":
            frames.append((int(f[1]), float(f[2]), float(f[3])))
        elif f[0] == "E":
            end = int(f[1])
    return ph, frames, end


def align(syms, stream):
    """start sample for each transcription symbol (None where the stream has nothing for it)"""
    n, m = len(syms), len(stream)
    INF = 1e9
    cost = [[INF] * (m + 1) for _ in range(n + 1)]
    back = [[None] * (m + 1) for _ in range(n + 1)]
    cost[0][0] = 0
    for i in range(n + 1):
        for j in range(m + 1):
            c = cost[i][j]
            if c >= INF:
                continue
            if i < n and j < m:                       # symbol i starts at stream j
                shows = SHOWS.get(syms[i], syms[i])
                d = 0 if stream[j][1][0] in shows else 1.2
                if c + d < cost[i + 1][j + 1]:
                    cost[i + 1][j + 1], back[i + 1][j + 1] = c + d, (i, j, "m")
            if j < m and i > 0:                       # an extra stream entry: part of the previous symbol
                shows = SHOWS.get(syms[i - 1], syms[i - 1])
                d = 0.1 if stream[j][1][0] in shows else 1.0
                if c + d < cost[i][j + 1]:
                    cost[i][j + 1], back[i][j + 1] = c + d, (i, j, "x")
            if i < n:                                 # a symbol the stream does not show (h, ...)
                d = 0.3 if syms[i] in "hH?" else 0.8
                if c + d < cost[i + 1][j]:
                    cost[i + 1][j], back[i + 1][j] = c + d, (i, j, "d")
    start = [None] * n
    i, j = n, m
    while (i, j) != (0, 0):
        pi, pj, op = back[i][j]
        if op == "m":
            start[pi] = stream[pj][0]
        i, j = pi, pj
    return start


def f0_at(frames, s):
    """F0 at output sample s: the frames' F0, with unvoiced stretches bridged"""
    lo, hi = 0, len(frames) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if frames[mid][0] <= s:
            lo = mid
        else:
            hi = mid - 1
    for d in range(len(frames)):
        for k in (lo - d, lo + d):
            if 0 <= k < len(frames) and frames[k][1] > 30:
                return frames[k][1]
    return 100.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("out")
    ap.add_argument("--pitch-scale", type=float, default=1.0)
    ap.add_argument("--keep", action="store_true", help="keep the phone script and Eloquence's own wav")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    timing = os.path.join(tmp, "eloq.txt")
    subprocess.run([os.path.join(ROOT, "build", "x64", "eloq_phon.exe"), a.text, timing,
                    os.path.join(tmp, "eloq.wav")], check=True, capture_output=True)
    stream, frames, end = read_timing(timing)
    words = parse_transcription(phonemes(a.text))
    syms = [s[0] for w in words for s in w if s != "."]
    starts = align(syms, [(t, c) for t, c in stream if c not in ("#", "\xa4")])
    pauses = [t for t, c in stream if c == "#"]

    # fill the symbols the stream skipped: take a slice of the next known phone
    known = [k for k, s in enumerate(starts) if s is not None]
    for k in range(len(syms)):
        if starts[k] is None:
            nxt = next((q for q in known if q > k), None)
            prv = max((q for q in known if q < k), default=None)
            t_next = starts[nxt] if nxt is not None else end
            t_prev = starts[prv] if prv is not None else 0
            starts[k] = t_prev + (t_next - t_prev) * 0.5 if prv is not None else max(0, t_next - 600)
    order = sorted(range(len(syms)), key=lambda k: starts[k])
    ends = {}
    bounds = sorted(set([s for s in starts] + pauses + [end]))
    for k in range(len(syms)):
        later = [b for b in bounds if b > starts[k]]
        ends[k] = later[0] if later else end
    lines, k = [], 0
    first = min(starts) if starts else 0
    if first > 200:
        lines.append("s %.4f %.1f" % (first / 11025.0, f0_at(frames, first) * a.pitch_scale))
    for w in words:
        lines.append("w")
        syl = []
        for s in w + ["."]:
            if s == ".":
                # a syllable: its phones, then its stress mark after the vowel, then a syllable break
                stress = max((st for _, st, _ in syl), default=0)
                for sym, st, kk in syl:
                    name = ELOQ.get(sym, sym)
                    if name not in SAPI:
                        print("unknown Eloquence phoneme %r - left out" % sym, file=sys.stderr)
                        continue
                    t0, t1 = starts[kk], ends[kk]
                    dur = max(0.02, (t1 - t0) / 11025.0)
                    f0s = [f0_at(frames, t0 + (t1 - t0) * q / 20.0) * a.pitch_scale for q in range(20)]
                    lines.append("p %d %.4f %s" % (SAPI[name], dur, " ".join("%.1f" % f for f in f0s)))
                    if sym in VOWELS and stress in (1, 2):
                        lines.append("m %d" % (8 if stress == 1 else 9))
                if syl:
                    lines.append("m 1")
                syl = []
            else:
                syl.append((s[0], s[1], k))
                k += 1
        if lines and lines[-1] == "m 1":
            lines.pop()
        # a pause after this word?
        last_end = max(ends[kk] for kk in range(k)) if k else 0
        for pz in pauses:
            if abs(pz - last_end) < 400:
                nxt = min([s for s in starts if s > pz], default=end)
                if nxt - pz > 330:
                    lines.append("s %.4f %.1f" % ((nxt - pz) / 11025.0, f0_at(frames, pz) * a.pitch_scale))
                break
    script = os.path.join(tmp, "phones.txt")
    open(script, "w").write("\n".join(lines) + "\n")
    subprocess.run([SAM_SAY, "--data", SAM_DATA, "--phones", "@" + script, a.out], check=True)
    if a.keep:
        base = os.path.splitext(a.out)[0]
        open(base + "_phones.txt", "w").write("\n".join(lines) + "\n")
        import shutil
        shutil.copy(os.path.join(tmp, "eloq.wav"), base + "_eloquence.wav")


if __name__ == "__main__":
    main()
