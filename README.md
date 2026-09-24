# Eloquence 6.1 in C

ETI Eloquence 6.1.0.2 (`ECI.DLL` + `ENU.SYN`, US English), rebuilt as C that produces the same samples,
with the engine's data compiled in: a drop-in `ECI.DLL` for x86 and x64 that needs no `.syn` or `.ini`
files.

**Status of the port.** Hand-written C: the formant synthesizer (`src/klatt.c`, the voice itself;
bit-exact on 19,902 recorded frames, `klatt_check`), the frame builder that feeds it (`src/framer.c`,
parameter tracks to 5 ms frames), the Delta rule runtime (`src/rules*.c`, 212 functions: the streams,
sync marks, pattern matching and backtracking the rules run on), the ECI API (`src/eci.c`), the runtime and maths.
Still recompiled from the machine code: the compiled Delta rule modules themselves (text normalization,
pronunciation, stress, intonation, durations) - these are replaced by hand-written C one function at a time (`x2c --replace`), each checked
against the recompiled one by `src/difftest.c` (snapshot the machine, run both, compare everything).

The engine is recompiled from its machine code (`tools/x2c.py`: every x86 instruction becomes C on a
software x86 with an exact 80-bit x87), the API layer is written by hand from the original's
decompilation (`notes/eci_api.md`). Everything is checked sample for sample against the real DLL.

**The Eloquence files are not in this repository and must not be added.** Generated sources
(`src/gen/`) contain the engine's code and data; they are made locally from your own copy.

## Building (Windows, Visual Studio, from PowerShell)

1. Put your `ECI.DLL`, `ECI.INI` and `ENU.SYN` (MD5 6c39acb7c1f0f62b3cae019f45b224fc) in `pkg/`.
2. Generate the C: `python tools/x2c.py --tree $(cat build/all_entries.txt) 0x10142e49 -o src/gen/enu.c --split 64 --hook 0x1013caf0`
   (the entry list comes from the Ghidra export, `ghidra/`) and `python tools/embed_image.py`.
3. `python tools/build_engine.py x64` and/or `python tools/build_engine.py x86`: the engine (64 files,
   about 10 minutes the first time), `build/<arch>/ECI.DLL`, `eloq_run.exe`, `ecitrace.exe`.
4. The reference tools (32-bit, they drive the real DLL): `harness\build.bat`.

## Using it

Copy `build/<arch>/ECI.DLL` where the application expects Eloquence's. It exports the same functions
with the same ordinals. Differences: only US English; on x64 the callback's `lParam` is pointer-sized.

`build/x64/eloq_run.exe - "Hello." out.wav` speaks one text (the default voice) into a file.

Extension voices: `eciCopyVoice(h, N, 0)` with N = 17 Robo, 18 Newsreader, 19 Vintage Reed, 20 Robo
Vibrato, 21 Newsreader Vibrato (names through `eciGetVoiceName`). They are ordinary voice parameters plus
effects on the synthesizer's frames and output (`src/voicefx.c`); copying a stock voice (1-8) turns the
effects off again, and without them the DLL is the original bit for bit.

Extension: `eciSetParam(h, eciSampleRate, 2 | 3 | 4)` runs the formant synthesizer itself at 22050,
44100 or 48000 Hz (0 and 1 are the original 8000 and 11025). For eloq_run: `ELOQ_RATE=48000`.

## Checking it

- `python tools/eci_compare.py`: every API script in `tests/eci/` through the real DLL and the port -
  calls, return values, callbacks with sample positions, and the samples, compared.
- `python tools/e2e.py tests/corpus.txt --voice 3 --syn -`: texts, one per line.
- `build\mathcheck.exe`, `build\fxtest.exe`: the maths against msvcrt and the x87 itself.

See `notes/port.md` for what was found and how, `notes/eci_api.md` for the API's behaviour.
