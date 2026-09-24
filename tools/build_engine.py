"""Incremental build of the recompiled engine: compile only the src/gen/enu_*.c files newer than their
objects (4 at a time, to keep memory use sane), then link eloq_run.

  python tools/build_engine.py [x64|x86]

Runs the Visual Studio environment through src/build_env.bat.
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    arch = sys.argv[1] if len(sys.argv) > 1 else "x64"
    src = os.path.join(ROOT, "src")
    out = os.path.join(ROOT, "build", arch, "enu")
    os.makedirs(out, exist_ok=True)
    stale = []
    hdr = os.path.join(src, "gen", "enu.h")
    for c in sorted(glob.glob(os.path.join(src, "gen", "enu_*.c"))):
        o = os.path.join(out, os.path.basename(c)[:-2] + ".obj")
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(c), os.path.getmtime(hdr)):
            stale.append(os.path.relpath(c, src))
    print("%d of the engine's files to compile" % len(stale))
    cf = "/nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /fp:precise"
    # the data compiled in (src/gen/enu_data.c from tools/embed_image.py, matched by the enu_*.c glob above)
    embedded = os.path.exists(os.path.join(src, "gen", "enu_data.c"))
    print("data:", "compiled in" if embedded else "read from ENU.SYN at run time (run tools/embed_image.py)")
    cmds = []
    if stale:
        cmds.append("cl %s /I. /MP4 /c /Fo..\\build\\%s\\enu\\ %s" % (cf, arch, " ".join(stale)))
    # klatt.c: the synthesizer, ported by hand (klatt_guest.c puts it where FUN_1013caf0 was)
    cmds.append("cl %s %s /Fo..\\build\\%s\\ x86rt.c fx80.c x87math.c crt.c image.c klatt.c klatt_guest.c eloq_run.c ..\\build\\%s\\enu\\*.obj "
                "/Fe:..\\build\\%s\\eloq_run.exe" % (cf, "/DELOQ_EMBEDDED" if embedded else "", arch, arch, arch))
    if embedded:
        # the ECI API as a drop-in ECI.DLL (the engine inside, no data files), and the script driver
        cmds.append("cl %s /Oy- /DELOQ_EMBEDDED /LD /Fo..\\build\\%s\\ eci.c eci_text.c voicefx.c engine.c klatt.c klatt_guest.c x86rt.c fx80.c "
                    "x87math.c crt.c image.c ..\\build\\%s\\enu\\*.obj /Fe:..\\build\\%s\\ECI.DLL "
                    "/link /DEF:eci.def winmm.lib user32.lib" % (cf, arch, arch, arch))
        cmds.append("cl %s /Fo..\\build\\%s\\ ..\\harness\\ecitrace.c user32.lib /Fe:..\\build\\%s\\ecitrace.exe" % (cf, arch, arch))
    bat = os.path.join(ROOT, "build", "build_engine_now.bat")
    with open(bat, "w") as f:
        f.write("@echo off\ncall \"%s\" %s\ncd /d \"%s\"\n" % (os.path.join(src, "build_env.bat"), arch, src))
        for cmd in cmds:
            f.write(cmd + " || exit /b 1\n")
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    errs = [l for l in r.stdout.splitlines() + r.stderr.splitlines() if "error" in l or "fatal" in l]
    print("\n".join(errs[:30]))
    print("build", "ok" if r.returncode == 0 else "FAILED")
    sys.exit(r.returncode)


main()
