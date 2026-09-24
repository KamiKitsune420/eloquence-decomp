"""The full regression against the real Eloquence, in parallel: tools/e2e.py on tests/corpus.txt in the 8
voice presets, tests/hard.txt, tests/escapes.txt and tests/long.txt, and tools/eci_compare.py for x64 and
x86. Prints each tool's summary line.

  python tools/regress.py [-j 8]
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd):
    r = subprocess.run([sys.executable] + cmd, capture_output=True, text=True, cwd=ROOT)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    return " ".join(cmd), r.returncode, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args()
    jobs = [["tools/e2e.py", "tests/corpus.txt", "--syn", "-", "--voice", str(v)] for v in range(1, 9)]
    jobs += [["tools/e2e.py", "tests/%s.txt" % t, "--syn", "-"] for t in ("hard", "escapes", "long")]
    jobs += [["tools/eci_compare.py"], ["tools/eci_compare.py", "--arch", "x86"]]
    ok = True
    with cf.ThreadPoolExecutor(a.j) as ex:
        for cmd, rc, lines in ex.map(run, jobs):
            print("%-55s %s" % (cmd, lines[-1] if lines else "(no output)"))
            for l in lines[:-1]:
                if "differ" in l or "fail" in l.lower():
                    print("    " + l)
            ok = ok and rc == 0
    sys.exit(0 if ok else 1)


main()
