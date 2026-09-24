"""Drive build/x64/ECI.DLL the way a screen reader's driver does (NVDA's IBMTTS add-on, _ibmeci.py):
a dedicated thread creates the handle, registers a WINFUNCTYPE callback and an output buffer, and runs a
GetMessage loop; speech requests arrive as thread messages; audio and index replies come through the
callback while that loop dispatches. Checks: every index arrives, eciStop cuts speech short and the
next text still speaks, nothing hangs.

  python tools/nvda_sim.py [out.wav]
"""
import ctypes
import os
import queue
import sys
import threading
import time
import wave
from ctypes import WINFUNCTYPE, byref, c_int, c_short, c_void_p, wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dll = ctypes.WinDLL(os.path.join(ROOT, "build", "x64", "ECI.DLL"))
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
for name, res, args in [("eciNewEx", c_void_p, [c_int]), ("eciDelete", c_void_p, [c_void_p]),
                        ("eciAddText", c_int, [c_void_p, ctypes.c_char_p]), ("eciInsertIndex", c_int, [c_void_p, c_int]),
                        ("eciSynthesize", c_int, [c_void_p]), ("eciStop", c_int, [c_void_p]),
                        ("eciSetOutputBuffer", c_int, [c_void_p, c_int, c_void_p]),
                        ("eciRegisterCallback", None, [c_void_p, c_void_p, c_void_p]),
                        ("eciSetParam", c_int, [c_void_p, c_int, c_int]),
                        ("eciSetVoiceParam", c_int, [c_void_p, c_int, c_int, c_int]),
                        ("eciCopyVoice", c_int, [c_void_p, c_int, c_int])]:
    f = getattr(dll, name)
    f.restype, f.argtypes = res, args

WM_SPEAK, WM_STOP, WM_QUIT_ = 0x401, 0x402, 0x12
BUF = 3300
samples, events, work = [], [], queue.Queue()
buf = (c_short * BUF)()


@WINFUNCTYPE(c_int, c_void_p, c_int, ctypes.c_ssize_t, c_void_p)
def callback(h, msg, lp, data):
    if msg == 0:
        samples.extend(buf[:lp])
        time.sleep(lp / 11025)            # a player that blocks at playback speed, as NVDA's does
    elif msg == 2:
        events.append(("index", lp, len(samples), time.time()))
    return 1


def eci_thread(ready):
    global handle
    handle = dll.eciNewEx(0x10000)
    dll.eciRegisterCallback(handle, callback, None)
    dll.eciSetOutputBuffer(handle, BUF, buf)
    dll.eciSetParam(handle, 1, 1)                 # annotations, as the driver sets them
    ready.set()
    msg = wintypes.MSG()
    while user32.GetMessageA(byref(msg), None, 0, 0) > 0:
        if msg.message == WM_SPEAK:
            for kind, value in work.get():
                if kind == "text":
                    dll.eciAddText(handle, value.encode("mbcs"))
                elif kind == "index":
                    dll.eciInsertIndex(handle, value)
            dll.eciSynthesize(handle)
        elif msg.message == WM_STOP:
            dll.eciStop(handle)
        else:
            user32.TranslateMessage(byref(msg))
            user32.DispatchMessageA(byref(msg))
    dll.eciDelete(handle)


def speak(tid, items):
    work.put(items)
    user32.PostThreadMessageA(tid, WM_SPEAK, 0, 0)


def main():
    ready = threading.Event()
    t = threading.Thread(target=eci_thread, args=(ready,), daemon=True)
    t.start()
    ready.wait(10)
    tid = t.native_id
    t0 = time.time()
    speak(tid, [("text", "`vs60 Welcome to the screen reader simulation."), ("index", 1),
                ("text", "This sentence will be interrupted before it ends, because the user pressed a key."),
                ("index", 2)])
    time.sleep(3.0)
    user32.PostThreadMessageA(tid, WM_STOP, 0, 0)
    time.sleep(0.5)
    n_after_stop = len(samples)
    speak(tid, [("text", "After stopping, speech continues."), ("index", 3), ("text", "Done."), ("index", 4)])
    deadline = time.time() + 30
    while time.time() < deadline and not any(e[1] == 4 for e in events):
        time.sleep(0.05)
    user32.PostThreadMessageA(tid, WM_QUIT_, 0, 0)
    t.join(10)
    idx = [e[1] for e in events]
    print("indices received:", idx)
    print("samples: %d (%.2f s of speech), %d at the stop, %.2f s wall time" %
          (len(samples), len(samples) / 11025, n_after_stop, time.time() - t0))
    ok = 1 in idx and 2 not in idx and 3 in idx and 4 in idx and not t.is_alive()
    print("OK" if ok else "PROBLEM")
    if len(sys.argv) > 1:
        with wave.open(sys.argv[1], "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(11025)
            w.writeframes((c_short * len(samples))(*samples))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
