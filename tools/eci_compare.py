"""The ECI API of the port against the real ECI.DLL: every script in tests/eci through both (harness/ecitrace.c),
the logs (calls, return values, callbacks with their sample positions) and the samples compared.

  python tools/eci_compare.py [script ...] [--arch x64|x86]
"""
import argparse
import difflib
import glob
import os
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(exe, dll, script, out):
    """the log, and the samples: the callback's, then any file the script had the DLL write (outfile)"""
    log, wav = out + ".log", out + ".wav"
    outfiles = [l.split(None, 1)[1].strip() for l in open(script) if l.startswith("outfile ")]
    for f in outfiles:
        if os.path.exists(f):
            os.remove(f)
    cmd = [exe] + (["--dll", dll] if dll else []) + [script, log, wav]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode:
        return None, None, r.stderr.strip() or "exit code %d" % r.returncode
    audio = open(wav, "rb").read()
    for f in outfiles:
        if os.path.exists(f):
            audio += open(f, "rb").read()
            os.remove(f)
    return open(log).read().splitlines(), audio, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scripts", nargs="*")
    ap.add_argument("--arch", default="x64")
    a = ap.parse_args()
    scripts = a.scripts or sorted(glob.glob(os.path.join(ROOT, "tests", "eci", "*.txt")))
    real = os.path.join(ROOT, "build", "ecitrace.exe")
    port = os.path.join(ROOT, "build", a.arch, "ecitrace.exe")
    dll = os.path.join(ROOT, "build", a.arch, "ECI.DLL")
    tmp = tempfile.mkdtemp()
    bad = 0
    for s in scripts:
        name = os.path.splitext(os.path.basename(s))[0]
        rl, rw, err = run(real, None, s, os.path.join(tmp, name + ".real"))
        pl, pw, perr = run(port, dll, s, os.path.join(tmp, name + ".port"))
        if err or perr:
            bad += 1
            print("%-12s FAILED: %s" % (name, err or perr))
            continue
        same_log, same_wav = rl == pl, rw == pw
        if same_log and same_wav:
            print("%-12s identical (%d log lines, %d bytes of audio)" % (name, len(rl), len(rw)))
            continue
        bad += 1
        print("%-12s DIFFERS: log %s, audio %s" % (name, "same" if same_log else "differs", "same" if same_wav else "differs"))
        for line in list(difflib.unified_diff(rl, pl, "real", "port", lineterm="", n=1))[:30]:
            print("    " + line)
    print("%d of %d scripts identical" % (len(scripts) - bad, len(scripts)))


if __name__ == "__main__":
    main()
