"""Eloquence voices after the old MacinTalk novelty voices, plus vibrato versions of Robo and Newsreader.

  python tools/mac_voices.py [outdir]

The Mac's Good News / Bad News / Cellos / Bells sang every syllable on the next note of a tune; Pipe Organ
and Trinoids were chords of voices; Zarvox a detuned, ring-modulated robot. Here the singing is done on
Eloquence's own synthesizer frames (voicefx tune=, a note per vowel), chords by mixing renders, and there
is a real vocoder too: the speech's spectral envelope on an organ-chord carrier that follows the melody.
"""
import os
import subprocess
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHON = os.path.join(ROOT, "build", "x64", "eloq_phon.exe")
RATE = 22050


def render(text, fx, path=None):
    """Eloquence at RATE with voice effects; returns (samples float, F0 track [(sample, hz, av)])"""
    tmp = os.path.join(os.environ.get("TEMP", "."), "macv")
    os.makedirs(tmp, exist_ok=True)
    wav, txt = path or os.path.join(tmp, "r.wav"), os.path.join(tmp, "r.txt")
    env = dict(os.environ, ELOQ_RATE=str(RATE))
    if fx:
        env["ELOQ_FX"] = fx
    subprocess.run([PHON, text, txt, wav], env=env, check=True, capture_output=True)
    with wave.open(wav) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64)
    track = [(int(l.split()[1]), float(l.split()[2]), float(l.split()[3])) for l in open(txt)
             if l.startswith("F") and len(l.split()) == 4]
    return x, track


def save(path, x):
    x = np.asarray(x, dtype=np.float64)
    peak = np.abs(x).max() or 1.0
    x = x * min(1.0, 30000.0 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.round(x).astype("<i2").tobytes())
    print(path)


def mix(parts, gains):
    n = max(len(p) for p in parts)
    out = np.zeros(n)
    for p, g in zip(parts, gains):
        out[:len(p)] += g * p
    return out


def f0_curve(track, n):
    """F0 per output sample (voiced frames only, held through the rest)"""
    t = np.array([s for s, hz, av in track], dtype=float)
    hz = np.array([h if av > 0 else 0 for s, h, av in track], dtype=float)
    last = 0.0
    for i in range(len(hz)):
        if hz[i] > 0:
            last = hz[i]
        else:
            hz[i] = last
    first = next((h for h in hz if h > 0), 110.0)
    hz[hz == 0] = first
    return np.interp(np.arange(n), t, hz)


def vocoder(mod, carrier, n_fft=1024, hop=256, lifter=40):
    """the modulator's spectral envelope (cepstrally smoothed) imposed on the whitened carrier"""
    win = np.hanning(n_fft)
    n = min(len(mod), len(carrier))
    out = np.zeros(n + n_fft)
    norm = np.zeros(n + n_fft)

    def envelope(frame):
        mag = np.abs(np.fft.rfft(frame * win)) + 1e-9
        cep = np.fft.irfft(np.log(mag))
        cep[lifter:-lifter] = 0
        return np.exp(np.fft.rfft(cep).real), mag

    for i in range(0, n - n_fft, hop):
        m_env, m_mag = envelope(mod[i:i + n_fft])
        c_spec = np.fft.rfft(carrier[i:i + n_fft] * win)
        c_env, _ = envelope(carrier[i:i + n_fft])
        loud = np.sqrt((m_mag ** 2).mean())
        y = np.fft.irfft(c_spec / c_env * m_env * (1 if loud > 1 else 0))
        out[i:i + n_fft] += y * win
        norm[i:i + n_fft] += win ** 2
    return out[:n] / np.maximum(norm[:n], 1e-3)


def organ_carrier(f0, ratios=(1, 1.5, 2, 3, 4), noise=0.03):
    """an additive organ: each note of the chord with a few harmonics, following f0 (per sample)"""
    t_phase = np.cumsum(f0) / RATE
    out = np.zeros(len(f0))
    for r in ratios:
        for h, amp in ((1, 1.0), (2, 0.5), (3, 0.35), (4, 0.2), (6, 0.12), (8, 0.08)):
            f = f0 * r * h
            out += amp / r * np.sin(2 * np.pi * r * h * t_phase) * (f < RATE / 2 - 500)
    return out + noise * np.random.default_rng(1).standard_normal(len(f0)) * np.abs(out).max()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "voices")
    os.makedirs(out, exist_ok=True)
    p = lambda name: os.path.join(out, name + ".wav")  # noqa: E731

    robo = "`vg0 `vh50 `vb40 `vf0 `vs55 "
    news = "`vg0 `vh65 `vb45 `vf45 `vr5 `vs48 "
    # vibrato versions of the two favourites
    render(robo + "Greetings, human. I am Robo, and now my voice has a wobble.", "mono=105,oq=20,bw=0.6,vib=35:6", p("vib_Robo"))
    render(robo + "Greetings again. This is Robo with a lot of vibrato, like a broken tape machine.", "mono=105,oq=20,bw=0.6,vib=90:4", p("vib_Robo_strong"))
    render(news + "Good evening. Here is the news, read with a gentle vibrato.", "formant=0.93,oq=48,bw=0.9,vib=22:5.5", p("vib_Newsreader"))
    render(news + "And now, the weather, read with far too much vibrato.", "formant=0.93,oq=48,bw=0.9,vib=70:6.5", p("vib_Newsreader_strong"))

    # the singers
    render("`vg0 `vh55 `vs45 Good news, everyone! The computer is working, and the coffee is hot. Everything is wonderful today.",
           "tune=goodnews,oq=40,vib=20:5.5", p("mac_GoodNews"))
    render("`vg0 `vh70 `vs40 I am sorry to tell you, the news today is very bad indeed. Your computer has crashed again.",
           "tune=badnews,oq=45,vib=15:5", p("mac_BadNews"))
    render("`vg0 `vh80 `vs55 Deep in the hall of the mountain king, the cellos are playing, and the trolls are dancing all night long.",
           "tune=cellos,oq=30,bw=0.8,vib=25:6", p("mac_Cellos"))
    render("`vg1 `vh30 `vs40 Ding dong, the bells are ringing, it is time to go home now.",
           "tune=bells,formant=1.1,oq=65,vib=10:4", p("mac_Bells"))

    # Pipe Organ: the hymn sung in three octaves and a fifth
    text = "`vg0 `vh60 `vs40 Praise the computer from whom all blessings flow, and praise the speech that tells us so."
    parts = [render(text, "tune=organ,oq=25,transpose=%d" % tr)[0] for tr in (-12, 0, 7, 12)]
    save(p("mac_PipeOrgan"), mix(parts, (0.9, 1.0, 0.7, 0.5)))

    # Trinoids: three voices, a chord that never moves
    text = "`vg1 `vh20 `vs50 We are the Trinoids. We come in peace. There are three of us, and we always agree."
    parts = [render(text, "mono=%g,formant=1.15,oq=35" % hz)[0] for hz in (196.0, 246.9, 293.7)]
    save(p("mac_Trinoids"), mix(parts, (1, 0.8, 0.8)))

    # Zarvox: two detuned monotone robots, ring-modulated
    text = robo + "Zarvox here. My circuits hum at a constant frequency. Please insert another floppy disk."
    a = render(text, "mono=98,oq=18,bw=0.5")[0]
    b = render(text, "mono=101,oq=18,bw=0.5")[0]
    z = mix([a, b], (1, 1))
    ring = np.sin(2 * np.pi * 38 * np.arange(len(z)) / RATE)
    save(p("mac_Zarvox"), 0.6 * z + 0.6 * z * ring)

    # a real vocoder: the sung hymn's envelope on an organ-chord carrier following its melody
    text = "`vg0 `vh60 `vs40 This is a vocoder. My voice is shaping the sound of an organ, playing a hymn."
    sung, track = render(text, "tune=organ,oq=25")
    carrier = organ_carrier(f0_curve(track, len(sung)) / 2)
    save(p("mac_Vocoder_Organ"), vocoder(sung, carrier))
    # and with a big fixed chord behind plain speech, the classic robot choir
    plain, track = render(robo + "And this is the robot choir. Every word is a chord.", "")
    chord = sum(organ_carrier(np.full(len(plain), hz), ratios=(1, 2)) for hz in (110.0, 138.6, 164.8))
    save(p("mac_Vocoder_Choir"), vocoder(plain, chord))


if __name__ == "__main__":
    main()
