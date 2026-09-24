r"""Incremental build of the recompiled engine: compile only the src/gen/enu_*.c files newer than their
objects (4 at a time, to keep memory use sane), then link eloq_run.

  python tools/build_engine.py [x64|x86] [difftest]

With `difftest`, also (only) the differential tester build\<arch>\difftest.exe (src/difftest.c): the
engine's files compiled again with -DX86_WTRACK into build\<arch>\enu_wt\, the hand ports with -DDIFFTEST.

Runs the Visual Studio environment through src/build_env.bat.
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# the hand ports and their adapters (the functions they replace are listed in src/ported.h)
PORTS = "klatt.c klatt_guest.c framer.c framer_guest.c rules.c rules_ops.c rules_delta.c rules_pool.c rules_edit.c rules_guest.c rules_io.c tracks.c"


def main():
    arch = sys.argv[1] if len(sys.argv) > 1 else "x64"
    difftest = "difftest" in sys.argv[2:]
    src = os.path.join(ROOT, "src")
    sub = "enu_wt" if difftest else "enu"
    out = os.path.join(ROOT, "build", arch, sub)
    os.makedirs(out, exist_ok=True)
    stale = []
    hdr = os.path.join(src, "gen", "enu.h")
    for c in sorted(glob.glob(os.path.join(src, "gen", "enu_*.c"))):
        o = os.path.join(out, os.path.basename(c)[:-2] + ".obj")
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(c), os.path.getmtime(hdr)):
            stale.append(os.path.relpath(c, src))
    print("%d of the engine's files to compile" % len(stale))
    cf = "/nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /fp:precise"
    if difftest:
        cf += " /DX86_WTRACK"
    # the data compiled in (src/gen/enu_data.c from tools/embed_image.py, matched by the enu_*.c glob above)
    embedded = os.path.exists(os.path.join(src, "gen", "enu_data.c"))
    print("data:", "compiled in" if embedded else "read from ENU.SYN at run time (run tools/embed_image.py)")
    cmds = []
    if stale:
        cmds.append("cl %s /I. /MP4 /c /Fo..\\build\\%s\\%s\\ %s" % (cf, arch, sub, " ".join(stale)))
    if difftest:
        os.makedirs(os.path.join(ROOT, "build", arch, "dt"), exist_ok=True)
        cmds.append("cl %s /DDIFFTEST %s /Fo..\\build\\%s\\dt\\ x86rt.c fx80.c x87math.c crt.c image.c %s difftest.c "
                    "..\\build\\%s\\enu_wt\\*.obj /Fe:..\\build\\%s\\difftest.exe"
                    % (cf, "/DELOQ_EMBEDDED" if embedded else "", arch, PORTS, arch, arch))
        return run(cmds, arch, src)
    # the hand ports (PORTS) put themselves where the recompiled functions were (src/ported.h)
    cmds.append("cl %s %s /Fo..\\build\\%s\\ x86rt.c fx80.c x87math.c crt.c image.c %s eloq_run.c ..\\build\\%s\\enu\\*.obj "
                "/Fe:..\\build\\%s\\eloq_run.exe" % (cf, "/DELOQ_EMBEDDED" if embedded else "", arch, PORTS, arch, arch))
    if embedded:
        # the ECI API as a drop-in ECI.DLL (the engine inside, no data files), and the script driver
        cmds.append("cl %s /Oy- /DELOQ_EMBEDDED /LD /Fo..\\build\\%s\\ eci.c eci_text.c voicefx.c engine.c %s x86rt.c fx80.c "
                    "x87math.c crt.c image.c ..\\build\\%s\\enu\\*.obj /Fe:..\\build\\%s\\ECI.DLL "
                    "/link /DEF:eci.def winmm.lib user32.lib" % (cf, arch, PORTS, arch, arch))
        cmds.append("cl %s /Fo..\\build\\%s\\ ..\\harness\\ecitrace.c user32.lib /Fe:..\\build\\%s\\ecitrace.exe" % (cf, arch, arch))
    return run(cmds, arch, src)


def run(cmds, arch, src):
    bat = os.path.join(ROOT, "build", "build_engine_now.bat")
    with open(bat, "w") as f:
        f.write("@echo off\ncall \"%s\" %s\ncd /d \"%s\"\n" % (os.path.join(src, "build_env.bat"), arch, src))
        for cmd in cmds:
            f.write(cmd + " || exit /b 1\n")
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    errs = [l for l in r.stdout.splitlines() + r.stderr.splitlines() if "error" in l or "fatal" in l]
    print("\n".join(errs[:30]))
    print("build", "ok" if r.returncode == 0 else "FAILED")
    return r.returncode


sys.exit(main())
