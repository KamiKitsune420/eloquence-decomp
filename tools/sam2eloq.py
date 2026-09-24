"""Microsoft Sam's front end, Eloquence's voice: Sam (the tts-random port) decides the words, syllables,
stress, phone durations and intonation; Eloquence's formant synthesizer makes the sound.

  python tools/sam2eloq.py "text" out.wav [--keep]

1. Sam, with SAM_DUMP: his words (SAPI phones, stress, syllables) and every sound's duration and F0.
2. Eloquence speaks Sam's phones as phonetic input (`[.0hE.1lo]); its frames and phoneme times are kept.
3. Each of Sam's sounds gets the frames of the matching Eloquence phoneme, stretched or squeezed to Sam's
   duration, with Sam's F0 written in; Sam's pauses become silent frames.
4. Eloquence's synthesizer is run again with those frames in place of its own (eloq_phon ELOQ_FRAMES_IN).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from eloq2sam import SAPI, SHOWS, align, read_timing  # noqa: E402

SAM = r"D:\llm-experiments\misk\tts-random"
SAM_SAY = os.path.join(SAM, "build", "x64", "sam_say.exe")
SAM_DATA = os.path.join(SAM, "data", "voice")
PHON = os.path.join(ROOT, "build", "x64", "eloq_phon.exe")

NAME = {v: k for k, v in SAPI.items()}
TO_ELOQ = {"iy": "i", "ih": "I", "ey": "e", "eh": "E", "ae": "A", "aa": "a", "ao": "c", "ow": "o", "uh": "U",
           "uw": "u", "ah": "H", "ax": "x", "er": "R", "ay": "Y", "aw": "W", "oy": "O", "th": "T", "dh": "D",
           "sh": "S", "zh": "Z", "ch": "C", "jh": "J", "ng": "G"}
FRAME_SEC = 0.005


def sam_dump(text, tmp):
    dump = os.path.join(tmp, "sam.txt")
    env = dict(os.environ, SAM_DUMP=dump)
    subprocess.run([SAM_SAY, "--data", SAM_DATA, text, os.path.join(tmp, "sam.wav")], env=env, check=True,
                   capture_output=True)
    words, items = [], []
    for line in open(dump):
        f = line.split()
        if f[0] == "W":
            words.append([int(x) for x in f[2:]])
        elif f[0] == "I":
            items.append((int(f[1]), int(f[2]), float(f[3]), [float(x) for x in f[4:24]]))
    return words, items


def eloq_word(ph):
    """Sam's word (SAPI ids; 8/9 stress after the vowel, 1 syllable break) as Eloquence phonetic input"""
    out, syl, stress = [], [], 0
    for p in ph + [1]:
        if p == 1:
            if syl:
                out.append(".%d%s" % (stress, "".join(syl)))
            syl, stress = [], 0
        elif p in (8, 9):
            stress = 1 if p == 8 else 2
        elif p in NAME:
            n = NAME[p]
            syl.append(TO_ELOQ.get(n, n))
    return "`[" + "".join(out) + "]"


def read_frames(path):
    d = open(path, "rb").read()
    n = len(d) // 260
    samples = np.array([struct.unpack_from("<i", d, 260 * i)[0] for i in range(n)])
    frames = np.frombuffer(b"".join(d[260 * i + 4:260 * i + 260] for i in range(n)), "<f4").reshape(n, 64).copy()
    return samples, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("out")
    ap.add_argument("--keep", action="store_true", help="also write Sam's own and Eloquence's own version")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    if a.keep:
        print("working files in", tmp)
    words, items = sam_dump(a.text, tmp)

    # Eloquence input: Sam's words, with a comma where Sam pauses between words
    parts, wi = [], 0
    for k, (ph, ws, dur, f0) in enumerate(items):
        if ws and wi < len(words):
            parts.append(eloq_word(words[wi]))
            wi += 1
        elif ph == 7 and dur >= 0.1 and parts and 0 < k < len(items) - 2:
            parts[-1] += ","
    end = a.text.rstrip()[-1:] if a.text.rstrip()[-1:] in ".?!" else "."
    phonetic = " ".join(parts) + end

    # pass 1: Eloquence speaks Sam's phones
    t1, f1 = os.path.join(tmp, "p1.txt"), os.path.join(tmp, "p1.frm")
    subprocess.run([PHON, phonetic, t1, os.path.join(tmp, "p1.wav")], env=dict(os.environ, ELOQ_FRAMES_OUT=f1),
                   check=True, capture_output=True)
    stream, _, total = read_timing(t1)
    fs, fr = read_frames(f1)
    sounds = [(k, it) for k, it in enumerate(items) if it[0] != 7]
    syms = [TO_ELOQ.get(NAME.get(it[0], "ax"), NAME.get(it[0], "x")) for _, it in sounds]
    starts = align(syms, [(t, c) for t, c in stream if c not in ("#", "\xa4")])
    bounds = sorted(set([s for s in starts if s is not None] + [t for t, c in stream] + [total]))
    span = {}
    for q, (k, it) in enumerate(sounds):
        s = starts[q]
        if s is None:                         # not in the stream (h, ...): just before the next one
            nxt = next((starts[r] for r in range(q + 1, len(sounds)) if starts[r] is not None), total)
            s = max(0, nxt - 400)
            span[k] = (s, nxt)
        else:
            span[k] = (s, next((b for b in bounds if b > s), total))
    silent = fr[-1].copy()
    silent[2] = 0.0
    silent[0] = 5.0

    # the new frame sequence: Sam's timing and pitch, Eloquence's spectra
    out, clock = [], 0.0
    for k, (ph, ws, dur, f0) in enumerate(items):
        n = int(round((clock + dur) / FRAME_SEC)) - int(round(clock / FRAME_SEC))
        clock += dur
        if n <= 0:
            continue
        if ph == 7:
            out.extend([silent] * n)
            continue
        a0, b0 = span[k]
        src = np.nonzero((fs >= a0) & (fs < b0))[0]
        if len(src) == 0:
            src = np.array([int(np.argmin(np.abs(fs - a0)))])
        for j in range(n):
            pos = (j + 0.5) / n * len(src) - 0.5
            lo = int(np.clip(np.floor(pos), 0, len(src) - 1))
            hi = min(lo + 1, len(src) - 1)
            w = float(np.clip(pos - lo, 0, 1))
            f = fr[src[lo]] * (1 - w) + fr[src[hi]] * w
            f[0] = 5.0                      # slot 0 is the frame's length in ms (Eloquence also uses 1 ms ones)
            f[1] = f0[min(19, int(20 * (j + 0.5) / n))] * 10.0
            out.append(f.astype("<f4"))
    frames_in = os.path.join(tmp, "p2.frm")
    np.array(out, "<f4").tofile(frames_in)

    # pass 2: Eloquence's synthesizer, given those frames (enough text for it to ask for them all)
    reps = int(len(out) / max(1, len(fr))) + 2
    subprocess.run([PHON, " ".join([phonetic] * reps), os.path.join(tmp, "p2.txt"), a.out],
                   env=dict(os.environ, ELOQ_FRAMES_IN=frames_in), check=True)
    print("%d of Sam's sounds, %d frames (%.2f s)" % (len(items), len(out), len(out) * FRAME_SEC))
    if a.keep:
        base = os.path.splitext(a.out)[0]
        shutil.copy(os.path.join(tmp, "sam.wav"), base + "_sam.wav")
        shutil.copy(os.path.join(tmp, "p1.wav"), base + "_eloquence_samphones.wav")
        open(base + "_phonetic.txt", "w").write(phonetic + "\n")


if __name__ == "__main__":
    main()
