"""New Eloquence voices: its own voice annotations (`vg gender, `vh head, `vb pitch, `vf fluctuation,
`vr roughness, `vy breathiness, `vs speed) plus frame effects past what they allow (eloq_run ELOQ_FX).

  python tools/make_voices.py [outdir] [--rate 48000]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, "build", "x64", "eloq_run.exe")

VOICES = [
    # name, annotations, frame effects, what it says
    ("Robo", "`vg0 `vh50 `vb40 `vf0 `vs55", "mono=105,oq=20,bw=0.6",
     "Greetings, human. I am Robo. My pitch never changes. Resistance is futile."),
    ("Whisper", "`v2 `vs45", "whisper,formant=1.05",
     "Psst. Come closer. This is Whisper. I have a secret to tell you."),
    ("Giant", "`vg0 `vh100 `vb0 `vf20 `vr20 `vs35", "formant=0.8,pitch=0.75,bw=1.3",
     "Fee, fie, foe, fum. I am the Giant, and I speak very slowly."),
    ("Squeaky", "`vg1 `vh0 `vb100 `vf80 `vs75", "formant=1.3,pitch=1.35",
     "Hi hi hi! I'm Squeaky! I'm tiny and I talk super fast!"),
    ("Opera", "`v2 `vf100 `vs40", "vib=55:5.5,formant=1.05",
     "Welcome to the opera. Every word I sing is dramatic, and trembles with emotion."),
    ("Demon", "`vg0 `vh100 `vb0 `vr100 `vy40 `vs38", "pitch=0.5,formant=0.78,breath=6",
     "You summoned me. I am the Demon of the default audio device."),
    ("Alien", "`vg1 `vh20 `vb70 `vs50", "formant=1.22,vib=35:11,oq=35",
     "Take me to your leader. My planet is very far away, and our vowels are different."),
    ("Nervous", "`vg0 `vh40 `vb60 `vf100 `vs70", "jitter=90,breath=4",
     "Um, h-hello? I'm Nervous. Is this thing on? I really hope nobody is listening."),
    ("Ghost", "`vg1 `vh60 `vb60 `vs35", "whisper,formant=1.1,vib=40:3",
     "Oooh. I am the Ghost of old screen readers past. Nobody uses my voice anymore."),
    ("Newsreader", "`vg0 `vh65 `vb45 `vf45 `vr5 `vs48", "formant=0.93,oq=48,bw=0.9",
     "Good evening. Here is the news. A text to speech engine from two thousand two has been rebuilt in C."),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(ROOT, "out", "voices")
    rate = sys.argv[sys.argv.index("--rate") + 1] if "--rate" in sys.argv else None
    os.makedirs(out, exist_ok=True)
    for k, (name, anno, fx, text) in enumerate(VOICES, 1):
        env = dict(os.environ, ELOQ_ANNOT="1", ELOQ_FX=fx)
        if rate:
            env["ELOQ_RATE"] = rate
        path = os.path.join(out, "new_%02d_%s.wav" % (k, name))
        subprocess.run([RUN, "-", anno + " " + text, path], env=env, check=True, capture_output=True)
        print(path)


if __name__ == "__main__":
    main()
