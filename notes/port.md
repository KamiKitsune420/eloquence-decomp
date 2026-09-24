# Eloquence 6.1 port - notes

Target: ETI Eloquence **6.1.0.2** as screen readers use it: `ECI.DLL` (32-bit, built 2006-02-10,
MD5 0cd344ebda29fdb3c05cec01d704504c) + `ENU.SYN` (32-bit PE, built 2002-07-30,
MD5 6c39acb7c1f0f62b3cae019f45b224fc). Goal: a C99 port that produces the same samples, with the
engine's tables compiled in (no data files at run time), packaged as an ECI-compatible DLL.

Files came from the user's EloquenceClone project (C:\Users\User\Documents\backup\TTS), whose notes are
background; openevv (IBM Embedded ViaVoice 4.3, decompiled from IBM's SDK objects) is the map for names.

## Sections

| binary | .text | .rdata | .data | notes |
|---|---|---|---|---|
| ECI.DLL | 111 890 | 9 605 | 47 904 | API, threads, hidden window, audio output, voice presets |
| ENU.SYN | 1 321 794 | 6 162 | 318 424 | the whole engine. Export `getObject`; DllMain at 0x10142e49 |

## Established

- **Oracle**: `build/ecisay.exe` (harness/ecisay.c, 32-bit) loads pkg/ECI.DLL + pkg/ECI.INI (paths rewritten
  by tools/make_ini.py) and captures every sample. 11025 Hz. **Deterministic** (identical MD5 over runs).
- **6.1 is not openevv**: openevv's evv.exe differs from sample 0 (corr 0.77, 19 415 vs 19 459 samples).
- **6.1's synthesizer is floating point** (`klatt_tl_table` as float32 at 0x10144910; openevv's is fixed
  point). x87 control word during synthesis: 0x027f (53-bit precision, round to nearest).
- **Pipeline bottom**: `FUN_101306b0` interpolates parameter tracks into frames of 62 floats (+ a leading
  samples-per-frame value) every 5 ms and calls the synthesizer `FUN_1013caf0(state, frame)` from one call
  site (0x10130a86). The synthesizer filters a block of 44/55 samples in float, converts it to int32 at
  state+0x70a (`FUN_1013c6c0`) and hands it on. Its output, concatenated, **is** the WAV ECI.DLL delivers.
  Frame layout: [0] samples/frame code, [1] F0*10, [2] AV, [3] open quotient (56), [9]/[10] F1/B1,
  [13]/[14] F2/B2, [15]/[16] F3/B3, [17].. F4.., fixed F5-F8 and bandwidths, parallel amplitudes 35-43.
- **The engine interface** (`getObject(1, &p)`, 14-byte wrapper, vtable 0x1014478c, 39 stdcall methods,
  names from openevv's EngineWrapper): QI, AddRef, Release, start, end, processSentences, processRemaining,
  getLastError, restart, readPhonemes, readErrorMessage, flush, clearInput, setAbort, outputPlaying, pause,
  setSynthToNamedFile, setSynthToCallback, setDurationCallback, register{Word,Index,Phoneme,Anno}Callback,
  insertSynthesisIndex, insertDelayedSynthesisIndex, wantPhonemeIndices, close, the dictionary methods,
  register{WordIndex,UserIndex}Callback.
- **ECI.DLL talks to the engine in annotations** (`ecisay --trace-engine`): voice setup is the text
  "`v1 `ts0 `da1 `ty1 `pp1" through processRemaining, then "`esr1" (twice), voice parameters that differ
  from the current voice as "`vg1" gender, "`vh22" head size, "`vb93" pitch base, "`vf35" pitch
  fluctuation, "`vr18" roughness, "`vy20" breathiness, "`vv90" volume (each via processSentences), then the
  text via processSentences, "" via processRemaining, flush(1), flush(0). Audio comes back through the
  setSynthToCallback function, cdecl (a, b, user).

## Method: recompiling the machine code (tools/x2c.py)

Ghidra's C loses x87 arithmetic entirely (bare `ftol()` calls) and was a source of errors in TruVoice, so
the engine is recompiled from its **machine code**: every instruction becomes C with exactly its effect on
a software x86 (src/x86rt.h): registers, lazily evaluated flags, a sparse 32-bit address space, the real
guest stack (calls push return addresses, rets pop them). The x87 runs on **src/fx80.c**, an exact 80-bit
extended implementation: arithmetic rounded to the precision control, float/double/integer stores with
subnormals, frndint, fscale, constants - all bit-identical to the chip (harness/fxtest.c, ~340k random
checks). x87 instructions are decoded from their bytes per the Intel manual (disassemblers swap some
fsub/fsubr forms). Imports go to src/crt.c; indirect calls through a table of all recompiled functions.

- **The synthesizer, recompiled, is bit-exact**: `synth_replay` feeds every recorded frame (state before,
  frame) and compares the whole state after and the samples: hello 354/354 frames; a long sentence in all
  8 voices, ~19 700 frames, 1.08 M samples, 0 differences.
- The x87 transcendentals (fsin/fcos/f2xm1) are **not** correctly rounded on this Intel i7-1355U (97%/97%/
  90% round-to-nearest, always within one ulp of 64 bits); fx80 rounds correctly. Not visible in any test
  so far (the results are rounded to 53/24 bits before use). If a difference appears: a table of the
  chip's answers for the arguments the engine uses.
- All 1811 functions Ghidra found recompile with no unsupported instruction (44 MB of C in 64 files).
- **The whole engine, recompiled, is bit-exact** (tools/e2e.py: ecisay against eloq_run, one process per
  line): tests/corpus.txt in all 8 voices, tests/hard.txt, tests/escapes.txt - 183 of 183 lines identical.

### ECI.DLL's text preparation (FUN_1000ddc5, before processSentences)

Bytes go through a table at 0x1001d3b0 (controls except \n, 0x7f, 0x81-0x90 but 0x85, 0x9b, 0x9d-0x9f,
0xff to space; 0x91/0x92 to '). A newline becomes a space unless a space precedes it (then it is
dropped). Escapes, from the strings at 0x1001d4b0: `|` -> `\| `, `^` -> `\\^ `, `\` -> `\\\\` (4), and with
annotations off (eciInputType 0, the default) `` ` `` -> ``\` ``. With annotations on a backtick stays an
annotation unless it starts `` `g ``, `` `i `` or `` `ui ``, and `\` before `\` or `` ` `` quotes it.
Implemented in eloq_run.c (`eci_prepare`, ELOQ_ANNOT=1 for annotations).

Voice presets (eciCopyVoice): each of gender, head, pitch, fluctuation, roughness, breathiness, speed,
volume that differs from Voice1 is sent as its own annotation `` `vg `vh `vb `vf `vr `vy `vs `vv `` in that
order (ELOQ_VOICE=n in eloq_run, --voice in e2e.py).

### log / exp / pow (src/x87math.c, harness/mathcheck.c)

They run while speaking (log, exp, pow ~ 1 : 9 : 0.3 per call to exp). msvcrt.dll's SSE2 versions are
**off** (`__use_sse2_mathfcns` at 0x100bfcec stays 0 unless _set_SSE2_enable is called), so its x87 code
runs and **leaves an extended-precision st0**, not a double:
- exp (0x100a6930 via _trandisp1, control word forced to 0x133f: 64-bit precision): t = x log2e, n =
  rndint t, f = t - n, 2^|f| = 1 + f2xm1|f|, reciprocal when f < 0, fscale by n.
- log (0x1009913f, control word 0x027f): fldln2, fyl2x.
- pow (0x10099404, caller's precision, round to nearest): fyl2x, then n = rndint t, f = -(n - t),
  (1 + f2xm1 f) 2^n; x < 0 needs an integer y (odd: negated).
- A double result that underflows or overflows goes through msvcrt's matherr machinery (not modelled:
  exact 0/inf from the host; the engine never gets there).
fx80 gained fyl2x (256-bit fixed point, atanh series; correctly rounded). Against msvcrt on the engine's
own arguments (ELOQ_MATHLOG over the whole test set): 0 differences as a double; 74 of 9 575 differ in the
last bit of 64 - the chip's fyl2x/f2xm1 are not correctly rounded (fyl2x 92% round-to-nearest). Random
arguments: exact as a double except where |y log2 x| is in the hundreds (then one ulp of fyl2x shows).
Beware: `_control87` takes Microsoft's abstract bits - `_control87(0x027f, ...)` sets the hardware word
to 0x0a7f (round up). Use `fldcw`.

### The data compiled in

The recompiled code never reads ENU.SYN's .text (checked by trapping every .text page); it needs .rdata
and .data only - 323 KB, written as C by tools/embed_image.py (src/gen/enu_data.c, checks the MD5, never
committed). With it, `eloq_run - "text" out.wav` and the DLL need no files at all.

### The ECI layer (src/eci.c, the drop-in ECI.DLL)

notes/eci_api.md is the specification (from ECI.DLL's decompilation). src/engine.c is one recompiled
engine (own memory; DllMain, getObject(2), start); src/eci_text.c the text escaping, character count,
annotation parser and real-world units (x87-exact via fx80); src/eci.c the API: a message window on the
eciNew thread, a worker thread per handle making every engine call, a dispatcher whose items run the
user callback from the window (message 0x500) or inside eciSpeaking/eciSynchronize, send-and-wait
waveform buffers, lazy sendParameters, index placement by character count, waveOut / wav-file output,
dictionaries through the engine's slots 27-36 (files via real stdio in crt.c). Exports and ordinals as
the original (src/eci.def). On x64 the callback's lParam is pointer-sized (LONG_PTR).

Checked with harness/ecitrace.c (a script of API calls; logs every return value and callback with its
sample position) against the real DLL: tools/eci_compare.py over tests/eci/*.txt - basic, indices,
several utterances, parameters, voices and presets, real-world units, annotations, text modes, 8 kHz,
manual mode, eciStop, eciReset, NotProcessed retries, tiny buffers, phoneme indices (ECIMouthData),
dense indices: logs and samples identical.

Also identical, x86 and x64 (23 scripts): dictionaries in memory and in files (saved files byte for
byte, reloading adds duplicates as the original does), .wav file output (byte for byte: the file stays
open across utterances, each utterance ends with one zero sample), the sound device (index replies when
the audio gets there, eciSynchronize after playback), callbacks through the window (`pump`).

Dictionary files go through the engine's MSVCIRT ifstream/ofstream (crt.c implements them with the
object layout the engine reads inline: vbtable -> ios at +0xc / +8, state at ios+8, _fGline at +4 - the
inlined getline sets it, get then takes the '\n') and SearchPathA + _stat. Beware when testing: the
32-bit reference process is UAC-virtualized under C:\Windows, the 64-bit port is not.

Deliberate differences: eciStop's flush(1) is made by the worker from its next engine callback (one
engine can't be entered from two threads); only US English 1.0 exists (eciGetAvailableLanguages lists
it alone); eciCopyVoice refuses the out-of-range targets the original writes out of bounds; the file
output is .wav whatever the extension; eciSynchronize also runs an item a previous eciSpeaking left.

### Extension: the synthesizer at 22050 / 44100 / 48000 Hz

The synthesizer's init FUN_1013c7f0 gets a config block by value (rate as a float at +4) and derives
every rate-dependent field from it: fs at state+0xa2e, pi/fs +0x14c7, 2pi/fs +0x14cb, fs/1000 +0x14cf,
1000/fs +0x14d3, and a mode byte +0x1ade (0 for 8000, 1 for 11025, 2 for anything else - room left for
another rate). The `esr annotation can't carry more than 32767 (the rule variable is a short), so the
init is recompiled with an entry hook (x2c --hook 0x1013caf0 0x1013c7f0) and the rate is overwritten
there (engine.c eng_set_rate; eloq_run ELOQ_RATE=48000). The formant synthesis then really runs at that
rate: same duration and pitch, levels within a couple of dB, content up to Nyquist (little of it: the
voice's energy above 5.5 kHz is ~37 dB down). ECI extension: eciSampleRate 2/3/4 = 22050/44100/48000
(0/1 as the original; the engine is told `esr1). x86 and x64 give identical 48 kHz output.

## Plan

1. ~~Run the whole recompiled engine and compare with ecisay~~ done; more: long texts, several utterances
   per session, annotations on, eciSetVoiceParam values in between the presets.
2. ~~log/exp/pow~~ done (x87math.c). atof/strtod: check they only parse the engine's own constants.
3. ECI layer by hand (the published ECI API over the engine interface, voices as annotations, the voice
   presets from ECI.INI), as a DLL.
4. Data compiled in: generator of the image's data sections (never committed).
5. Size/speed: peephole the generated C (cmp+jcc, register caching) once everything is exact.
