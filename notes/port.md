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

### Hand ports in place of recompiled functions, and the differential tester

A hand port replaces a recompiled function by address: src/ported.h lists them (`PORTED(addr, flags)`),
tools/x2c.py `--replace-list src/ported.h` renames the recompiled one to f_ADDR_recomp, and the port
defines f_ADDR through PORT_FN (src/port.h: cdecl arguments with ARG(n), port_ret). Ports work in guest
memory and call other engine functions through the machine at the esp the original has (a callee's
frame, arguments written into argument slots, and the stack garbage it reads are then the same).

- the synthesizer (klatt.c) and frame builder (framer.c), with adapters klatt_guest.c / framer_guest.c
- the rule runtime (0x10130e80-0x10136000 and its helpers up to 0x1013a000): rules.c, rules_ops.c,
  rules_delta.c, rules_pool.c, rules_edit.c, rules_io.c (adapters in rules_guest.c or at the file's end),
  tracks.c; the data structures (streams, elements, sync marks, values, refs, control stack, workspace)
  are documented in src/rules.h

**src/difftest.c** (`python tools/build_engine.py x64 difftest`, run by `python tools/difftest.py
[--quick] [--only addr,...]`): every call of a ported function runs the recompiled original on a snapshot
of the machine first, records the result, takes its writes back (guest memory writes are tracked in
256-byte blocks, a build with X86_WTRACK), runs the port, and compares eax (per its flags: all / al /
none; edx for 64-bit results), ebx esi edi ebp esp, the x87 state, the heap, and the memory either one
wrote at or above the entry esp (below it is the callee's scratch). Nested ported calls are compared at
every level; host state (audio, voice effects, maths counters) is snapshotted too; a longjmp out of a
call is caught (x86_longjmp_hook) and compared as well. DIFFTEST=all|none|addr,addr (default all),
DIFFTEST_VERBOSE=n (mismatches printed in full). tools/retuse.py justifies the eax flags (what the callers read), tools/argwrites.py
finds functions that write their argument slots, tools/regress.py runs the whole e2e + eci_compare
regression in parallel.

### The compiled Delta rules, lifted (tools/delta_lift.py, src/lift.h)

The 999 functions that call `_setjmp3` and rl_enter (0x101315e0) are the compiled Delta rules. delta_lift
decodes each with x2c's decoder and executes its basic blocks symbolically (esp as an offset from the
entry, ebp the frame pointer, locals kept at the original's guest addresses because the runtime holds
pointers into the frame, registers as C expressions, flags as the last flag-setting instruction, join
variables at control-flow merges). The output is structured C calling the runtime by name (r_enter,
r_var_init, r_backtrack, ...) and other rules as f_ADDR through the machine at the original's esp
(lift.h: S(off, ret), lcN). x87 instructions run on the machine's fx80 as x2c emits them. Anything
outside the model is refused and stays recompiled. Output: src/gen/rules_lifted_NN.c, lifted.h (the
replace list), lift_rt.h, lift_report.txt (all git-ignored, derived from ENU.SYN).

Regenerate: `python tools/delta_lift.py`, then x2c with `--replace-list src/ported.h --replace-list
src/gen/lifted.h`, then build_engine.py.

993 of 999 are lifted. The 6 refused (10001001, 100010ff, 10001e05, 100044ff, 10031302, 100c151e) are not
functions: they are Ghidra entry points inside other functions (10001001 = 10001000 without its `push
ebp`), reachable only from the address table, never called.

Constructs the lifter handles that needed care (2026-09-25):
- `push ecx` used as stack filler before an x87 `fstp qword [esp]` (for exp/pow arguments): the register
  is undefined after a call, so the slot gets the machine's c->ecx (what the original writes too), and at
  a join an undefined ecx/edx on one path becomes c->ecx. Checked: those values are only ever filler.
- exp / log / pow: called as imports on the machine (arguments already stored on the stack by the x87,
  result in st0), the same x87-exact implementations as the recompiled code (x87math.c).
- `neg ax; sbb eax, eax` (CF = operand != 0), `sar dx, 8` / `imul ax, ax, 3` on 16-bit registers, `and
  edx, 0xff` after `mov dl, ...` (the mask defines the undefined upper bits), `lea` over a register with
  undefined upper bits when only its low part is used later.

The machine's registers mirror the original's at every call, setjmp and return (`c->esi = eng;` ... emitted
only where a value changes; ebx/esi/edi kept across calls, eax/ecx/edx forgotten after one; entry values of
the callee-saved registers are the constants in_ebx/in_esi/in_edi/in_ebp). Without it the lifted code was
right but left different stack bytes: a recompiled callee's prologue saves ebx/esi/edi/ebp and MSVC's
`push ecx` reserves a local with ecx, and the rules copy uninitialized stack bytes into the heap (the tail
of a value struct that a memset only partly clears), so difftest reported 28 000 mismatches (audio
unaffected). With the mirroring, and the runtime run recompiled (DIFFTEST_RTRECOMP), the lifted rules
differ from their originals only where the original itself was wrong (next paragraph).

x2c bug found this way (fixed 2026-09-25): emit_fn writes a function's blocks by address and started
executing at the lowest one, so a function whose reachable code goes below its entry (68 of 1811: mostly
Ghidra entries inside other functions, reached by a jump that x2c turns into a tail call, e.g.
f_100c1948 from FUN_100c151d) ran from the wrong instruction. Now it starts with `goto L_entry`.

Status 2026-09-25 13:54 (993 rules lifted, x2c fixed): regress 195/195 lines + 23/23 ECI scripts x64 and x86
identical to the real engine; x86 and x64 eloq_run identical on corpus + hard; difftest with
DIFFTEST_RTRECOMP=1: 436 cases, 7 878 905 checked calls, 0 mismatches. Plain difftest (hand ports in):
28 418 mismatches in 19 rules, all uninitialized stack bytes the hand-ported runtime leaves differently
below its esp, copied into the heap by the rules; audio identical in all 436 cases. To remove them the
hand ports would have to reproduce their originals' scratch writes.

Builds: `ELOQ_MP=2` (env) runs 2 compiler processes instead of 4 - the generated files need ~1 GB each,
and 4 at once ran the laptop out of memory.

difftest debugging switches (src/difftest.c): DIFFTEST_RTRECOMP=1 runs the hand ports (not the lifted
rules) as their recompiled originals, so a lifted rule is compared with the same callees on both sides
(the hand ports leave different scratch below their esp by design); DIFFTEST_WATCH=addr reports every
write to that address's 256-byte block (writer's esp, the code addresses on the stack above it, and the
written value on the next line, WROTE); DIFFTEST_WATCH_RS=1 watches the rule cursor (RS + 0xfc6) of the
checked call; DIFFTEST_CALLLOG=1 logs every hand-ported function's call inside a check on both sides
(address, return address, two arguments, eax) - diff the orig and port lines to find where they part.

### The hand ports' scratch made exact (done 2026-09-25)

The rules copy uninitialized stack bytes into the heap, so a hand port must leave below its entry esp the
bytes its original leaves. DIFFTEST_SCRATCH=1 makes difftest compare that scratch too (reported as
[esp-N]); run it over the hand ports only:
`$env:DIFFTEST_SCRATCH="1"; python tools/difftest.py --quick --only <all of ported.h>` - a function's count
includes its hand-ported callees' scratch, so fix callees first. The recipe, per function (read its
machine code with tools/pushes.py, tools/disasm.py: pushes, calls, their esp):
1. the prologue's register saves: generated for every hand port by tools/prologues.py (src/gen/prologues.c,
   port_prologue called by PORT_FN) - registers pushed at entry, interleaved loads allowed;
2. saves the original makes later on some path (e.g. rl_enter's push ebx/edi after the early return,
   rl_goto_a_at's `push ebp` around the stream check): written by hand where they happen;
3. engine functions the original calls are called through the machine (call2/call3/call_at from
   rules_int.h, at the original's esp before its pushes, with its return address), not as C functions -
   the callee's adapter then writes its own frame;
4. around those calls the machine's registers hold the original's values (callees save them: `esi = eng`
   and the like, from the listing), restored before returning (the caller's contract).
5. a hand port written as a whole C module (klatt.c, framer.c) runs its frame where the original has it and
   records the locals the original leaves (their final values; later code reads nothing else), and the
   registers the original holds where it calls out (the engine's callbacks save them).
DIFFTEST_SHOW=n lists n differing bytes per report (default 6).

Status (evening 2026-09-25): every hand port leaves the original's stack scratch. The last ones:
- split_at 10136320: registers ebp = eng, edi = s, esi = s & 0xff, ebx = the token next to m; left/right
  in its locals at sp-0x14/-0x10; the final call with esi = 19 * (s & 0xff). (This also made the insert_list
  and get_sv residuals exact: their garbage came from split_at's frame.)
- the frame builder 101306b0 (framer.c): its calls through the machine where the original makes them - the
  stop check 10142350 (ret 0x101308c7), the track accessors 1012f960 / 1012f980 (the calloc count's call
  made with the size already pushed), the next breakpoint 1012f8a0 with ebx = eng, esi = the segment,
  edi = the track, ebp = the cursor; its locals (step as a qword, the interpolation's dt/dv/v0/x, t, the
  slot/track loop pointers, the failure flag, the last frame's length); at the synthesizer call ebx = eng,
  esi = t (or t - end + step for the frame past the end: the original reuses esi), edi = t + step,
  ebp = end.
- the queue functions: 1012f8a0 calls 10130bd0 / 10130c60 on the machine (entry in its local at esp+8,
  bl = 1, esi = the queue); 10130c60 tail-calls 10130d40 from its entry esp; 10130d40 makes the C-runtime
  calls below its three (four, while unwrapping a ring) saves. Its eax: (capacity & 0xff00) | 1, or the new
  buffer | 1, or 0 - visible now that the calls go through the machine.
- the synthesizer 1013caf0 (klatt.c + klatt_guest.c): klatt.c fills an optional klatt_trace (NULL for other
  hosts): its frame's final bytes - freq[k] at +0x8c + 4k, bw[k] at +0x38 + 4k, par_db at +0xe0 (the same
  arrays as its synth struct), nsamp +0x2c, remember_from +0x28, the aspiration gain +0x24, first_block
  (byte) +0x1f, the block's remaining samples +0x30, the parallel loop's k +0x20 and sign +0x14, the x87
  scratch +0x10 (n * 1000, then the pitch period in samples, the tilt C, each whole period's length), +0x18
  (the samples filtered first, then the last noise sample, then a pointer to par_db[nres]), the bypass gain
  +0x34; and at each output the registers ebx (the output pointer after the int loop, or n for a silent
  block), edi (n, or 0), ebp (0 for silence; &par_db[nres] after the parallel branch; else the glottal
  source's position, or the buffer pointer to the closed part after a partial period). klatt_guest.c writes
  the frame, saves edi at +0, and makes FUN_1013c6c0's frame (count, state, return 0x1013e83f, edi; with
  output on ebx, esi, {count, samples}) and calls the engine's callback from it (ret 0x1013c728) with ebx,
  ebp, esi = edi = state. Not covered: frames with no samples (nsamp <= 0 leaves the ftol/div calls'
  scratch, which the port does not write), and with output off the bytes below FUN_1013c6c0's frame that
  the filters' calls leave (with it on, the callback's frame covers them) -
  neither is exercised by the tests.

### The whole engine hand-written (user's decision, evening 2026-09-25)

Replaces the earlier "rules rewritten by hand, same memory" plan. Everything still machine-derived becomes
hand-written C: the 993 lifted rules (median 123, max 6021 Ghidra lines; 225 568 in all) and the functions
still recompiled - 287 direct call targets (~17k Ghidra lines, build/remaining_real.txt; 1012b..10143: the
engine interface, callbacks, sentence processing, audio/synth management, CRT helpers; 10120212 is 3290
lines alone) plus the ones reached only through pointers (ELOQ_ICALLS=file logs indirect targets: ~38 stubs
at 10128d89..10128fcc, the callbacks 1012d480..1012de00, 1012ef80, 1012f2b0, 1012f560, CRT 10142d40 /
10142e49). The other ~310 entries x2c emits are Ghidra's fake mid-instruction entries (never run).

Contract (user's choice): output-exact, clean C. Audio and the ECI API stay sample-identical; the engine's
memory stays identical except bytes the original leaves uninitialized (the tester masks them; any that do
reach the output are reproduced deliberately and documented). New code carries no return addresses or
register echoes. The end state deletes the software x86, x2c and the lifter from the build.

## Plan

1. ~~Run the whole recompiled engine and compare with ecisay~~ done; more: long texts, several utterances
   per session, annotations on, eciSetVoiceParam values in between the presets.
2. ~~log/exp/pow~~ done (x87math.c). atof/strtod: check they only parse the engine's own constants.
3. ECI layer by hand (the published ECI API over the engine interface, voices as annotations, the voice
   presets from ECI.INI), as a DLL.
4. Data compiled in: generator of the image's data sections (never committed).
5. Size/speed: peephole the generated C (cmp+jcc, register caching) once everything is exact.
