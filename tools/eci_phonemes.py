"""Eloquence's own phoneme transcription of a text (eciGeneratePhonemes through build/x64/ECI.DLL).

  python tools/eci_phonemes.py "text"
"""
import ctypes
import os
import sys
from ctypes import WINFUNCTYPE, c_int, c_void_p

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def phonemes(text, dll_path=os.path.join(ROOT, "build", "x64", "ECI.DLL")):
    dll = ctypes.WinDLL(dll_path)
    dll.eciNew.restype = c_void_p
    for n, a in [("eciSetParam", [c_void_p, c_int, c_int]), ("eciAddText", [c_void_p, ctypes.c_char_p]),
                 ("eciGeneratePhonemes", [c_void_p, c_int, c_void_p]), ("eciDelete", [c_void_p]),
                 ("eciRegisterCallback", [c_void_p, c_void_p, c_void_p])]:
        getattr(dll, n).argtypes = a
    buf = ctypes.create_string_buffer(4096)
    out = []

    @WINFUNCTYPE(c_int, c_void_p, c_int, ctypes.c_ssize_t, c_void_p)
    def cb(h, msg, lp, data):
        if msg == 1:
            out.append(buf.raw[:lp].decode("latin-1"))
        return 1

    h = dll.eciNew()
    dll.eciRegisterCallback(h, cb, None)
    dll.eciSetParam(h, 0, 1)                # manual mode, which eciGeneratePhonemes needs
    dll.eciAddText(h, text.encode("mbcs"))
    dll.eciGeneratePhonemes(h, len(buf), buf)
    dll.eciDelete(h)
    return "".join(out)


if __name__ == "__main__":
    print(phonemes(sys.argv[1]))
