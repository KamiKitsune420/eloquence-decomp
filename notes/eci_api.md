# ECI.DLL (Eloquence 6.1.0.2) - behavioural specification of the public ECI API

Source: Ghidra decompilation `decomp/ECI/_all.c` of pkg/ECI.DLL, plus machine code where Ghidra lost the
x87 (disassembled with capstone) and constants/strings read straight from the DLL image. Every claim cites the
function(s) it comes from. **[V]** = read in the code; **[I]** = inferred (reasoning, not a direct read);
**[?]** = ambiguous or depends on the engine, and should be checked by tracing.

Engine vtable slots are written `eng.name(args)` = `(**(*obj + 4*slot))(obj, args)`, stdcall. Slots used:
3 start, 5 processSentences, 6 processRemaining, 9 readPhonemes, 11 flush, 12 clearInput, 17
setSynthToCallback, 19 registerWordCallback, 20 registerIndexCallback, 21 registerPhonemeCallback, 22
registerAnnoCallback, 23 insertSynthesisIndex, 24 insertDelayedSynthesisIndex, 25 wantPhonemeIndices, 26
close, 27 newDict, 29 setDict, 30 deleteDict, 31 loadDict, 32 saveDict, 33 updateDict, 34 dictFindFirst, 35
dictFindNext, 36 dictLookup, 37 registerWordIndexCallback, 38 registerUserIndexCallback, 2 Release.
Never called by ECI.DLL: 1 AddRef, 4 end, 7 getLastError, 8 restart, 10 readErrorMessage, 13 setAbort, 14
outputPlaying, 15 pause, 16 setSynthToNamedFile, 18 setDurationCallback, 28 getDict. (AddRef, which a trace
saw, is not in ECI.DLL's code; it probably happens inside the engine's `getObject`. [?])

All exports are `__stdcall` (e.g. `ret 8` at the end of eciGetParam, `ret 4` for eciVersion).

---------------------------------------------------------------------------------------------------------

## 0. Architecture in one page

ECI.DLL has three layers:

1. **Public ECI layer** (exports, 0x10001c0d-0x1000693f): owns the 0x646-byte handle, ECI parameters, voices,
   voice presets, the "manual mode" text queue, error bits, callback translation (FUN_100069ee).
2. **Internal "eci*2" layer** (FUN_1000943f..FUN_1000a410, log names `eciNew2`, `eciAddText2`, `eciSynthesize2`,
   `eciPoll2`, ...): thin wrappers around a 0xb1-byte object (the `eci2` object, handle+0xc) that holds
   - `+0x00` the **synthesis controller** (C++ object, 0x106 bytes, vtable 0x1001d644, ctor FUN_10010754), which
     *is a thread* (the synthesis worker) with a command queue;
   - `+0x04` the **callback dispatcher** (FUN_10001000, vtable 0x1001d1b0): a queue of callback items going
     back to the API thread, announced with `PostMessage(hwnd, 0x500, hECI, 0)`;
   - `+0x4c` the **parameter mirror** (20 ints + mutex, FUN_1000ab65): the engine-side values, kept current by
     the engine's annotation callback;
   - `+0xa5` the text splitter (FUN_10015ced; handles `` `l `` language switches).
3. **Audio output layer**: a device manager (global DAT_1002b984), per-device objects with their **own audio
   thread**, a global 30 ms "ticker" thread, and output drivers `wav`, `au`, `snd`, `ral`, `rau` (files) and
   `dev` (winmm waveOut), FUN_1000e747..FUN_1001b780.

Every engine call made while synthesizing happens **on the synthesis worker thread** (commands executed from
its queue). A few setup calls (output buffer/format registration) are made directly on the calling thread.
The user callback runs on the thread that polls the dispatcher (section 2).

---------------------------------------------------------------------------------------------------------

## 1. Data layout

### 1.1 The ECI handle (malloc(0x646), memset 0; eciNew FUN at 1000225d, eciNewEx 10001dd1) [V]

The structure is byte-packed (odd offsets exist).

| off | type | meaning | evidence |
|---|---|---|---|
| 0x000 | HWND | message window ("eciWindowETI", 50x50, WS_CHILD of the pump window if there is one, else top-level); **created by, and owned by, the thread that called eciNew** | eciNew |
| 0x004 | HWND | pump thread's top-level window (0 once the thread has ended) | FUN_10009137 |
| 0x008 | HANDLE | pump thread | eciNew |
| 0x00c | ptr | eci2 object (layer 2) | FUN_1000943f |
| 0x010 | fn | user callback (eciRegisterCallback) | eciRegisterCallback |
| 0x014 | ptr | user data | same |
| 0x018 | int[17] | ECI parameters, index i at 0x18+4i (1.3) | eciGetParam/eciSetParam |
| 0x05c | int[17] | "last sent to the engine" copy of the parameters, same indexing | FUN_10007e05, FUN_10007f6a |
| 0x0a0 | voice | active voice (voice 0) | eciCopyVoice |
| 0x0ec | voice | "last sent to the engine" voice | FUN_10007d0c, FUN_100082ec |
| 0x138 | voice[8] | user voices 9..16 (0x138 + (n-9)*0x4c) | eciCopyVoice |
| 0x398 | int | output buffer size in samples (eciSetOutputBuffer) | eciSetOutputBuffer |
| 0x39c | int | backup of 0x398 | FUN_10008cbd |
| 0x3a0 | ptr | output buffer | eciSetOutputBuffer |
| 0x3a4 | ptr | backup of 0x3a0 | FUN_10008cbd |
| 0x3a8 | int | last index delivered (eciGetIndex) | FUN_100069ee cases 0, 8 |
| 0x3ac | int | output mode: 0 device or file, 1 output buffer, 2 phoneme buffer, 3 pinyin buffer, 4 none | FUN_10006dd4, FUN_10006fd5, FUN_10006f20, FUN_100070fc |
| 0x3b0/0x3b4/0x3b8/0x3bc/0x3c0 | | dictionary results: value ptr / key ptr / value size / key size / part of speech | dictionary exports |
| 0x3c4 | char[0x100] | output device ("%d") or file name | eciSetOutputDevice, eciSetOutputFilename |
| 0x4c4 | char[0x100] | backup of 0x3c4 | FUN_10008cbd/FUN_10008cff |
| 0x5c4 | ECIMouthData (0x16 bytes) | lParam target of eciPhonemeIndexReply | FUN_100069ee case 3 |
| 0x5da / 0x5de | ptr | manual-mode queue head / tail | FUN_100075ea |
| 0x5da + L*8 + D*4 | ptr | dictionary currently set for language L (1..10), dialect D (0..1); first used slot 0x5e2 | FUN_10008d60, FUN_10008e2c |
| 0x632 | byte | "resend all parameters" (set by eciStop) | eciStop, FUN_1000905e |
| 0x634 | uint | status bits (eciProgStatus) | FUN_10008ea8 |
| 0x638 | uint | last error code (eciErrorMessage) | FUN_10008ea8 |
| 0x63c | byte | reentrancy flag | all guarded exports, FUN_10006955 |
| 0x63d | ptr | text preprocessor ("romanizer" wrapper, controller+0xf6) | FUN_10009506 |
| 0x641 | ptr | wide string returned by the Ex dictionary functions | FUN_10005c12 |
| 0x645 | byte | 1 when created by eciNewEx2 (no default audio device) | eciNewEx2 |

### 1.2 Voice record, 0x4c bytes, packed [V]

| off | meaning |
|---|---|
| 0x00 | name, char[30]; byte 0x1e is forced to 0 by eciSetVoiceName |
| 0x1f | int[8] voice parameters in ECI units: 0 gender (0..1), 1 head size, 2 pitch baseline, 3 pitch fluctuation, 4 roughness, 5 breathiness (0..100 each), 6 speed (0..250), 7 volume (0..100). Ranges are the table at 0x1001d230 = 0x1001d348 |
| 0x3f | byte, "preset defined" (only meaningful in the preset table) |
| 0x40 | int, real-world pitch (Hz) |
| 0x44 | int, real-world speed (words/min) |
| 0x48 | int, real-world volume |

Real-world conversions (FUN_10001923; x87 read from the machine code; `_ftol` truncates toward zero):
- pitch: `trunc( 2.0 ** (v * 0.06306456) * 4.889761232 + 35.11023877 + 0.01f )` (0 -> 40 Hz, 100 -> 422)
- speed: `trunc( (v*1.406 + 70.25) + (v*v)*0.014 + 0.5 )` (v*v is an integer product) (0 -> 70, 250 -> 1297)
- volume: `trunc( 1.11728696759 ** v )` (0 -> 1, 100 -> 65535)

The value is clamped to the voice-parameter range before converting. FUN_10003d7e(voice, -1) recomputes all
three real-world fields; FUN_10003d7e(voice, p) only the one for p in {2,6,7}. The inverse (FUN_10001a29) is a
binary search over 0..250 (clamped to the range) for the ECI value whose converted value is nearest the
requested real-world value (with 2 candidates left it picks the nearer; ties go to the lower one).

### 1.3 ECI parameters (index -> handle 0x18+4i) [V]

Ranges: the table at 0x1001d2c0 (pairs min,max), checked in eciSetParam. Defaults: FUN_10007e05 (runs in eciNew
and eciReset), with the other values left at 0 by the memset.

| # | name | range | default | notes |
|---|---|---|---|---|
| 0 | eciSynthMode | 0..1 | 0 | 1 = manual: eciAddText/eciInsertIndex queue until eciSynthesize |
| 1 | eciInputType | 0..1 | 0 | 1 = annotations are interpreted |
| 2 | eciTextMode | 0..3 | mirror[0] | `` `ts `` |
| 3 | eciDictionary | 0..1 | mirror[3] | **stored inverted**: Set stores 1-v, Get returns 1-stored (a stored value other than 0/1 reads as 0). Stored value = the engine's `` `da `` value |
| 4 | (undocumented) | 0..100 | 0 | stored, never used |
| 5 | eciSampleRate | 0..1 | 1 | 0 -> 8000, 1 -> 11025 (22050 is not accepted) |
| 6 | (undocumented) | 0..100 | 0 | stored, never used |
| 7 | eciWantPhonemeIndices | 0..1 | mirror[4] | |
| 8 | eciRealWorldUnits | 0..1 | 0 | changes voice parameter units and rewrites `` `vb `vs `vv `` numbers in annotated text |
| 9 | eciLanguageDialect | valid installed L.D | mirror[2] | (L<<16)\|D; 0x10000 = US English; bit 0x800 = UCS-2 (Unicode) dialect |
| 10 | eciNumberMode | 0..1 | mirror[1] | `` `ty `` |
| 11 | eciPhrasePrediction | 0..1 | mirror[0xd] | `` `pp `` |
| 12 | eciWantWordIndex | 0..1 | 0 | only effective if the language's CallbackFlag has 0x100 (the shipped INI has 0x3f, so it does nothing) |
| 13 | eciNumDeviceBlocks | 2.. | 10 | eciSetParam refuses (-1) |
| 14 | eciSizeDeviceBlocks | 220.. | 2200 | refused |
| 15 | eciNumPrerollDeviceBlocks | 0.. | 0 | refused |
| 16 | eciSizePrerollDeviceBlocks | 220.. | 2200 | refused |
| 17 | eciNumParams | - | - | Get/Set return -1 |

"mirror[k]" is the eci2 parameter mirror (eci2+0x4c, read by FUN_1000b551). Its entries are written (a) by the
engine's **annotation callback** FUN_10014037(id, value, user) -> FUN_1000b517(mirror, id, value), and (b)
directly: [2] = the language when an engine is attached (FUN_10010e96, FUN_100135ec), [4] = the
wantPhonemeIndices value (FUN_10010e96, FUN_1001344f). Mirror ids: 0 ts, 1 ty, 2 dialect, 3 da, 4
wantPhonemeIndices, 5..12 voice g h b f r y s v, 13 pp; `+0x50` (byte) "phoneme-buffer mode". So the defaults
of params 2, 3, 10, 11 and of the active voice are **whatever the engine reports through the annotation
callback** after the init string "`v1 `ts0 `da1 `ty1 `pp1" [V for the mechanism; the values are **[?]**: expected
ts 0, da 1 (so eciGetParam(3)=0), ty 1, pp 1, voice = the engine's `v1; check with `getparam`/`getvparam`].

### 1.4 Global (per-process) state [V]

| address | meaning |
|---|---|
| DAT_1002b93c | number of live handles (under the mutex DAT_10028858) |
| DAT_10020030 | "window class not registered yet" (starts at 1). **One flag for both class names**: the first eciNew/eciNewEx registers "eciWindowETI", eciNewEx2 registers "eciWindow" (register-once bug, section 8) |
| DAT_10028730 | the INI object (whole file in memory, or registry fallback), FUN_10016cf0 |
| DAT_10028860 | voice preset table, 0x2fd0 bytes: per language L (1..10), dialect D (0..1) a 0x264-byte block at 0x10028860 + (L-1)*0x4c8 + D*0x264: int "section present", then 8 voice records (preset n at 0x10028864 + (L-1)*0x4c8 + D*0x264 + (n-1)*0x4c) |
| DAT_1002b984 | audio device manager + 30 ms ticker thread (created with the first controller, freed with the last; DAT_1002b98c counts controllers) |
| DAT_1002b988 | phoneme -> mouth-shape tables (from the INI "PhonemeN" keys), for eciPhonemeIndexReply |
| DAT_1002bac0[] | audio drivers, registered in the order wav, au, snd, ral, rau, dev (FUN_10017c80) |
| DAT_1002b970 | debug log: opened with "r+" on "eci.dbg" in the current directory at load; if the file exists, every FUN_1000b7dd call is logged. Nothing else depends on it |

**INI file** (FUN_10016cf0): path = directory of the module loaded as "eci.dll" + "\\" + "eci.ini". If no module of
that name is loaded: `%ECIINI%`, otherwise cwd + "\\eci.ini". If the file can't be read: registry
`HKLM\Software\SpeechWorks International, Inc.\ETI-Eloquence-mfW\ECIINI` (subkeys as sections). The file is read
whole and ended with 0xff (FUN_10017400).

**Voice presets** (FUN_10007627/FUN_10007716, at DLL load): the table is zeroed, then for every section named
"[L.D]" (sscanf "%d.%d" must return 2; "[LanguageIndependent]" is skipped): the section is marked present; for n =
1..8 the key `Voice<n>` is split on spaces and parsed with "%d" into the 8 parameters, **without a range
check**; the real-world fields are computed (FUN_10003d7e -1); name = "Adult Male 1", "Adult Female 1", "Child
1", "Adult Male 2", "Adult Male 3", "Adult Female 2", "Elderly Female 1", "Elderly Male 1" for n = 1..8; the
defined byte (+0x3f) is set to 1. Missing keys leave that preset undefined (all zero).

Other INI keys: `Path` (engine DLL, FUN_1000bd88), `CallbackFlag` (hex, "%10x"; default 0x3f when absent,
FUN_1000c01d/FUN_1000bd3e), `Path_Rom` (romanizer DLL for languages 6/8/10, FUN_1000d9ef), `PhonemeN` (mouth
data, FUN_1000cf9b, section 5.5).

**Engine loading** (FUN_1000ba1e): LoadLibrary(Path) with the current directory temporarily set to the DLL's
own directory; `getObject(2, &obj)`; `obj.start()` (slot 3); if start returns nonzero: `close()` (26),
`Release()` (2), FreeLibrary. **Each ECI handle loads its own engine object** (the controller's table at
controller+0x43, FUN_1000be17). Engine for language L.D = section "[L.D]" (FUN_1000bc41).

---------------------------------------------------------------------------------------------------------

## 2. Threads and message flow

### 2.1 Threads per handle [V]

1. **Pump thread** (FUN_10009137, one per handle, started by eciNew*): creates a top-level "eciWindowETI" window
   with the desktop as owner, then runs `GetMessage/TranslateMessage/DispatchMessage` until it gets WM_QUIT.
   It does no ECI work; its window is only the *parent* of the handle's message window (so that window is a
   WS_CHILD, presumably to keep it out of top-level window broadcasts [I]). eciNew waits up to about 1 s
   (201 x Sleep(5)) for the window to exist. Ended by `SendMessage(pumpwnd, WM_CLOSE, 0x7a49, -0x7a4a)`
   (FUN_10006955: DestroyWindow + PostQuitMessage) and waited for up to 10 s.
2. **Synthesis worker** (the controller, FUN_1000fc35 -> FUN_10016c00 `_beginthreadex(FUN_10016cc0)`; main loop
   FUN_1000cdf7): takes commands from its queue (controller+0x11) and runs `cmd->vtable[6]` one at a time; after
   each one it sets the "idle" event (+0x3a). Commands are posted asynchronously (FUN_10010282 -> FUN_1000ca85)
   and each post adds to "pending" (controller+0x67) and to the dispatcher's "posted sequence" (dispatcher+0x35),
   and sets "busy" (controller+0x62) - see 3.x for the amounts.
3. **Audio thread** per output device or file (FUN_1000f1ae, same thread class): runs audio commands 0x3e8-0x3f1
   (open, write samples, insert marker, flush, pause, stop, close, query state), each sent synchronously by the
   worker (FUN_1000fdcf = send and wait).
4. **Ticker thread**, one per process (FUN_100161e4 in DAT_1002b984, main FUN_10016584): while at least one device
   is playing it posts a "poll position" command (0x3ef -> driver +0x49) to each device's audio thread every 30
   ms; that is how index markers placed in the audio stream fire (section 6.1).

### 2.2 The callback dispatcher [V]

Synthesis-side events become dispatcher items (FUN_100010a4 posts without waiting; FUN_1000117d posts and
**blocks the posting thread until the item has been executed**). Posting an item = push it on the
dispatcher's queue + `PostMessage(hwnd = handle[0], 0x500, wParam = hECI, 0)` (FUN_1000ca85).

Items are executed only by **eciPoll2** (FUN_10009bab -> FUN_10012e8d -> FUN_100013e1) or **eciSynchronize2**
(FUN_100098f4 -> FUN_10012e62 -> FUN_1000128f), on whatever thread calls them:
- The message window's procedure (FUN_10006955) runs eciPoll2 for message 0x500 or WM_TIMER (0x113) when
  wParam != 0, with the handle's reentrancy byte set around it. The window belongs to the thread that called
  eciNew, so **callbacks run on that thread when it dispatches messages**.
- eciSpeaking and eciSyncWait call eciPoll2 directly; eciSynchronize (and eciNewDict, eciGeneratePhonemes,
  eciSpeakText internally) call eciSynchronize2: **callbacks run inside these calls, on the caller's thread**.

Executing an item = `FUN_100069ee(eci2, type, data, hECI)`, which translates it into the user callback (5.1).
Its result goes in item+0x1a: 0 = processed, -1 = the user returned eciDataNotProcessed.

eciPoll2 (FUN_100013e1), returns:
- if paused (dispatcher+0x3d): 6 if posted == completed, else 1.
- if posted sequence (+0x35) == completed sequence (+0x39): 2 (idle).
- otherwise: first retry the *retained* item (+0x3e) if there is one: run it again; if it returns 0, release
  it, clear +0x3e and KillTimer; if still -1, return 3. Then loop: take the next item without waiting (none -> 3;
  queue failure -> -2); remove one pending 0x500 message with `PeekMessage(hwnd,0,0,PM_REMOVE)`; a **type-7
  item** (end marker) sets completed = its data, and if completed == posted returns 4 after draining all
  messages of the window (FUN_1000173d); any other item is executed: -1 -> keep it as the retained item, start
  `SetTimer(hwnd, id = hECI, 30 ms)`, return 3; otherwise release it (which wakes a blocked poster) and go on.
- 4 also clears the controller's busy flag.

eciSynchronize2 (FUN_1000128f): 1 if paused, 2 if idle; otherwise loop **waiting** on the queue's
"non-empty" event; type-7 items as above (return 4 when complete); other items are executed and, while they
return -1, the loop Sleeps 30 ms and runs them again (so NotProcessed makes eciSynchronize busy-retry). It does
not look at a retained item left by eciPoll2 **[?]**.

The -0x12 ("abort") paths in both functions are dead: FUN_100069ee returns only 0 or -1. So
**eciDataAbort (2) is treated like eciDataProcessed** [V: FUN_100069ee checks `== 0` only].

### 2.3 How the API calls use them

- eciAddText / eciInsertIndex / eciSynthesize post commands to the worker and return at once (3.4).
- eciSpeaking = one eciPoll2: TRUE for results 3 or 1 (1 = paused with work pending), FALSE otherwise.
- eciSynchronize = eciSynchronize2: returns only when the end marker of the last posted command arrives (or at
  once when paused/idle).
- eciStop runs FUN_10012c0c **on the calling thread** (3.x: it blocks the queues, waits for the worker to
  finish its current command, flushes the engine, empties everything).
- eciPause affects callback delivery (dispatcher pause) and the wave device (waveOutPause/Restart).
- eciClearInput only empties the manual-mode queue (no engine, worker or dispatcher effect).

If the application neither dispatches messages nor calls eciSpeaking/eciSynchronize, the worker stays blocked in
the first waveform (or phoneme buffer) callback it posts, because those are send-and-wait [V: FUN_10013f27,
FUN_10013fdf -> FUN_1000117d].

---------------------------------------------------------------------------------------------------------

## 3. Exports

Common patterns:
- **Reentrancy guard** (most exports): `if (h && h->reenter) { h->lastErr = 0x800; h->status |= 0x800; return
  failure; }`. Functions that also *set* the flag for their duration: eciReset, eciStop, eciSpeaking,
  eciSynchronize, eciGeneratePhonemes, eciGeneratePinyins, eciDelete (and the window procedure around eciPoll2).
  So inside a user callback that was delivered through the window, eciSpeaking or eciSynchronize, every guarded
  function fails with 0x800. Unguarded: eciGetParam, eciGetVoiceParam, eciGetVoiceName, eciVersion,
  eciProgStatus, eciErrorMessage, eciGetIndex, eciSyncWait, eciSpeakText(Ex), eciGetAvailableLanguages, all
  dictionary functions, logging stubs, eciIsBeingReentered.
- **Error mapping** of internal results (FUN_10008ea8), setting lastErr (0x638) and OR-ing status (0x634):
  -1 -> 0x1; -2 -> 0x2; -3, -6, -7, -8 -> 0x80; -4, -5, -0x10 -> 0x20; -9 -> 0x100; -0xd -> 0x200; -0xe, -0x15 ->
  0x4000; -0xf -> 0x10; others (-0xa, -0xb, -0xc, -0x11..-0x14, >= 0) -> unchanged, no error set.
  Callback-side errors (FUN_100069ee): type 3 with NULL data or type 4 -> 0x10; 5 -> 0x20; 6 -> 0x2; 9 ->
  0x1000; 10 -> 0x4000. Engine errors (type 4, FUN_10015a6e) and romanizer errors (9/10, FUN_10015c0c) are
  reported once until eciClearErrors; an audio error (type 5, FUN_100158f9) also closes the device and sets the
  controller's audio-error flag (+0xe5), after which audio is dropped until eciClearErrors.
- "Returns TRUE/FALSE": the low byte of eax; the upper bytes hold junk (e.g. `CONCAT31(x, 1)`) [V].

### 3.1 Creation / deletion

**eciNew()** (1000225d) -> ECIHand or NULL [V]:
1. lock DAT_10028858, count++, register the window class "eciWindowETI" (wndproc FUN_10006955) if not done yet.
2. malloc/zero the handle; start the pump thread; wait for its window (<= 201 x 5 ms); create handle[0]
   ("eciWindowETI", WS_CHILD of the pump window, or top-level if there is none).
3. handle[0xb] (= param 5, sample rate) = 1; `FUN_1000943f(&h->eci2, 0)`: new eci2 object -> new controller
   (worker thread starts) -> FUN_1000ad35(mirror, controller, 0): the language = the INI section with the
   **lowest name by strcmp** ("[1.0]" with the shipped INI) -> FUN_10010e96 = engine setup:
   ```
   (load engine if needed: LoadLibrary, getObject(2,&e), e.start())
   e.wantPhonemeIndices(0); mirror[4] = 0
   e.registerAnnoCallback(FUN_10014037, ctrl)
   if (flags & 0x200) e.registerUserIndexCallback(0, ctrl) else if (flags & 1) e.registerWordCallback(0, ctrl)
   e.processRemaining("`v1 `ts0 `da1 `ty1 `pp1")          ; nonzero -> -0xf
   FUN_100131b2:
     if (flags & 0x200) e.registerUserIndexCallback(FUN_100152c0, ctrl)
     else if (flags & 1) e.registerWordCallback(FUN_100152a9, ctrl)
     if (flags & 0x100) e.registerWordIndexCallback(0, ctrl)
     e.registerIndexCallback(FUN_10015c90, ctrl)
     e.registerPhonemeCallback(FUN_10015ca7, ctrl)
     e.registerAnnoCallback(FUN_10014037, ctrl)
     e.setSynthToCallback(FUN_10015c76, ctrl)                ; nonzero -> fail
     e.clearInput()                                          ; nonzero -> fail
     e.flush(0)                                              ; nonzero -> fail
   mirror[2] = language; FUN_1000e372(preproc, 2, language) -> (no romanizer) FUN_10013571:
     e.processRemaining(NULL)
   ```
   (flags = the language's CallbackFlag, 0x3f -> the word-callback branch.) This matches the traced order;
   the traced `processRemaining("")` is a **NULL pointer** in the code [V: FUN_10013571 passes 0].
4. FUN_10007e05 (parameter defaults, 1.3), FUN_10007d0c (active voice = mirror[5..12], real-world fields,
   name "Adult Male 1"; copied to the last-sent voice), manual queue empty, 0x632 = 0.
5. User voices 9..16 = copies of presets 1..8 of the current L.D, renamed "User-Defined".
6. `FUN_10009a81(eci2, FUN_100069ee, h, 0x500, h->hwnd)`: the dispatcher's callback = FUN_100069ee, user
   argument = eci2, timer id/wParam = h, message = 0x500, window = handle[0].
7. `eciSetOutputDevice(h, -1)` (section 6) -> wave mapper; this makes the first
   `registerWordCallback(0) / processRemaining("`esr1") / registerWordCallback(FUN_100152a9)` (FUN_10012538).
   Failure -> eciNew fails.
8. h->0x63d = the text preprocessor. Any failure: tear everything down, return NULL.
(The second traced `esr1` group comes from the caller's eciSetOutputBuffer, see 3.6.)

**eciNewEx(dialect)** (10001dd1): like eciNew, but handle 0x3c (param 9) = dialect before step 3, and the eci2
object is created for that dialect (FUN_1000a55c -> FUN_1000ad35(..., dialect)); no engine for it -> NULL. The
preset/user-voice copy uses that dialect.

**eciNewEx2()** (100026d9, no argument): like eciNew (default language) but the window class is "eciWindow",
and instead of eciSetOutputDevice(-1) it calls FUN_100070fc (output mode 4 = **no output**) and sets 0x645 = 1.
No audio device is opened; audio is discarded unless eciSetOutputBuffer/Filename/Device is called.

**eciDelete(h)** (10002b5e) -> always NULL. Guarded (flag byte at h+0x63c, read as `param_1[399]`). Sets the
flag, then FUN_10009540 (deletes eci2: the controller's destructor FUN_10010af3 runs FUN_10012c0c = stop, i.e.
`e.flush(1)`, `e.flush(0)` (3.5); stops the worker; releases the device; FUN_1000bebb releases every engine
holder: `e.close()`, `e.Release()`, FreeLibrary), DestroyWindow(handle[0]), WM_CLOSE to the pump window, wait
<= 10 s, frees the manual queue, frees the handle, count--. (The traced `flush(1), flush(0)` at the end of a
text come from this stop path, or from eciStop/eciReset - they are **not** part of synthesizing a text [V].)

**eciReset(h)** (10002ce1) -> BOOL. Sets the reentrancy flag. `eciReset2` = FUN_1000a6ba: FUN_10012c0c (stop)
then FUN_1000ad35 again (FUN_10010e96: the engine setup of 3.1 step 3 **without** loading/start, since the
engine is already loaded). Then: free the manual queue, FUN_10007e05 + FUN_10007d0c (defaults again - param 5
back to 1), 0x632 = 0, user voices reset, backup of the output settings; if not NewEx2: output = device
"-1" with the current parameters (FUN_10006dd4) - **a registered output buffer is dropped**; on success clear
0x634/0x638, clear the flag, return TRUE. NewEx2 handles: FUN_100070fc, and then the code falls through to the
"NULL handle" error return: **returns FALSE and leaves the reentrancy flag set** (bug, section 8).

### 3.2 Information / errors

- **eciVersion(buf)**: `sprintf(buf, "%d.%d.%d.%d", 6, 1, 0, 2)` -> "6.1.0.2" (DAT_10022938..44). NULL buf: nothing.
- **eciProgStatus(h)**: h->0x634; NULL -> 0x80.
- **eciErrorMessage(h, buf)**: text for h->0x638: 0 "", 1 "System error.", 2 "System resources are low.", 0x10
  "Synthesis engine error.", 0x20 "Audio device error.", 0x80 "Invalid or out of range parameter.", 0x100 and
  0x2000 "Synthesis engine is busy.", 0x200 "Audio device busy.", 0x400 "Synthesis engine is paused.", 0x800
  "Cannot reenter ECI on the same thread.", 0x1000 the romanizer's message ("No Romanizer Error" if there is
  none), 0x4000 "Cannot load module.", anything else "". NULL h: "System resources are low." In a UCS dialect
  (param 9 & 0x800) the text is widened to UTF-16. NULL h also takes the widening path and dereferences NULL
  (crash) [V].
- **eciClearErrors(h)**: guarded; eciClearErrors2 (FUN_10012e2b: clears the once-only engine/romanizer error
  flags and the audio-error flag, romanizer clear), then 0x638 = 0x634 = 0.
- **eciGetIndex(h)**: h->0x3a8 (last eciIndexReply or eciWordIndexReply lParam; 0 initially); NULL -> 0.
- **eciGetAvailableLanguages(langs, &n)**: returns 0x80 if &n is NULL, n < 0, or langs NULL with n != 0.
  Enumerates INI sections; each parsing as "L.D" gives (L<<16)|D. n == 0 on entry: n = number found. Otherwise
  fills up to n entries in section order, n = number written. Returns 0.
- **eciIsBeingReentered(h)**: the flag byte (0 for NULL).
- **eciDialogBox()** -> TRUE, no UI. **eciRequestLicense(code)** -> no-op (log only). **eciStartLogging,
  eciStopLogging, eciSynchronizeSynth** -> no-op. **eciGetLog, eciGetIntLog** -> 0. No license checks anywhere [V].

### 3.3 Parameters and voices

**eciGetParam(h, p)** (10003606): NULL h or p outside 0..16 -> -1; p == 17 -> -1; else the value (p == 3
inverted). No errors set, not guarded.

**eciSetParam(h, p, v)** (1000368d): guarded (-> -1). Returns -1 for NULL h, p outside 0..16, v outside the
range; p == 9 also needs FUN_100091b9 (L in 1..10, D in 0..1, section present). p 13..16 -> -1 (not stored).
Otherwise stores v (p == 3: 1-v) and returns the previous value (p == 3 as the caller sees it). **No engine
call; no error bits.** The change reaches the engine at the next sendParameters (3.4) - i.e. at the next
eciAddText or eciInsertIndex in normal mode, or when the queued items are sent at eciSynthesize in manual mode
(where each item carries the parameters current at the time it was queued).

**eciGetVoiceParam(h, voice, p)** (10003e33): -1 for NULL h, p == 8, voice outside 0..16, p outside 0..7. voice 0
= active, 1..8 = preset of the *current* L.D (zeros if not defined), 9..16 = user voices. With param 8
(RealWorldUnits) == 1, p 2/6/7 return the real-world fields.

**eciSetVoiceParam(h, voice, p, v)** (10003ff2): guarded. Only voice 0 or 9..16 (presets are read-only), p
0..7, v within the range - in real-world units the range limits are converted too (pitch 40..422, speed
70..1297, volume 1..65535). Returns the old value (in the units in use) or -1. Units 0: store v, recompute the
real-world field. Units 1: store the real-world value (p 2/6/7), and ECI value = inverse (FUN_10001a29, 0..250).
**No engine call**; sent at the next sendParameters.

**eciCopyVoice(h, from, to)** (10003815): guarded. Accepted when (to is 0 or 9..16 and from == 0) or (from is
9..16, or from is 1..8 with that preset defined). **When from is 1..16, `to` is not checked** [V: operator
precedence of the condition]; to in 1..8 then writes to h+0x138+(to-9)*0x4c, i.e. before the user voices (into
the handle's own fields, or before the handle if to < 7 - memory corruption). The whole 0x4c record is copied
(name, flags, real-world fields). Returns TRUE/FALSE. **No engine call.** When the active voice changed, the
next eciAddText/eciInsertIndex sends every parameter that differs from the last-sent voice, each as its own
command `processSentences("`vgN")` ... in the order g h b f r y s v (FUN_100082ec) - the traced behaviour.

**eciGetVoiceName(h, voice, buf)** (100039d3): voice 0..16 -> copies the name (widened in UCS dialects), TRUE.
NULL buf -> error 0x80 (and with a NULL h, a write through NULL). voice outside 0..16 -> FALSE, no error.

**eciSetVoiceName(h, voice, name)** (10003b8e): guarded. voice 0 or 9..16: strncpy 30 chars, byte 30 = 0. Other
voices: ignored, **still returns TRUE**. NULL h -> FALSE (but it calls eciGetParam(NULL,9) = -1 first, whose
0x800 bit selects the UCS conversion with a NULL handle - probable crash).

### 3.4 Text flow

**sendParameters** (FUN_1000905e), used by eciAddText and eciInsertIndex in normal mode and per item at
eciSynthesize in manual mode. force = h->0x632 (set by eciStop; cleared after a successful send):

FUN_10007f6a(params, force) compares each with the last-sent copy (0x5c+4i) and, if force or different, acts
**in this order** (each engine action is a separate worker command unless noted; the last-sent value is
updated after each success; the first failure aborts sendParameters):
1. params 5, 13..16 (sample rate, device blocks) differ or force -> re-establish the output, **on the calling
   thread**: mode 0 -> FUN_10006dd4 (new audio format, 6); mode 1 -> FUN_10006fd5 (re-register the buffer:
   `e.registerWordCallback(0)`, `e.processRemaining("`esr0" | "`esr1")`, `e.registerWordCallback(FUN_100152a9)`);
   mode 4 -> just record. Refused with -9 (0x100) while busy.
2. param 9 (dialect) -> language command (FUN_10011f36 -> worker FUN_100135ec, 3.9). Notes whether the language
   (L) changed.
3. param 2 -> annotation command "`ts%u" **then** a set-param command (0, v) (FUN_10011ddb -> worker
   FUN_100134ba -> FUN_1000e372(0,v): if v != 0, `e.processRemaining(NULL)`).
4. param 3 (stored value) -> "`da%u".
5. param 11 -> "`pp%u".
6. param 7 -> command 0x7d3 (FUN_1001344f): `e.processRemaining(NULL)` (via FUN_10013571), then
   `e.wantPhonemeIndices(v)`, mirror[4] = v.
7. param 9 again - only when force (it was just updated otherwise): a second language command.
8. param 10 -> "`ty%u".
9. param 12 -> set-param command (0xe, v) if force, language changed, or different: `e.registerWordIndexCallback`
   only with CallbackFlag 0x100; then if v != 0, `e.processRemaining(NULL)`.
Then FUN_100082ec(voice, force): for ids 5..12 = voice parameters 0..7 (g h b f r y s v): if force or
different from the last-sent voice, post "`v?%u" and record it. Annotation strings are built by FUN_1000b648:
`sprintf("`%s%u", name, value)`, posted as an annotation command (FUN_10011c34) whose worker side is
`e.processSentences(copy)` (FUN_10012f69 -> FUN_1000d8ee -> FUN_10013080), **not escaped**.

So after eciStop, the next eciAddText (normal mode, English, buffer output) sends: the esr group (1), then on
the worker: processRemaining(NULL) (language), "`ts<v>", "`da<v>", "`pp<v>", processRemaining(NULL) +
wantPhonemeIndices(v), processRemaining(NULL) (language again), "`ty<v>", all 8 voice annotations, then the
text [V; order from FUN_10007f6a/FUN_100082ec].

**eciAddText(h, text)** (10004393) -> BOOL:
1. guarded; NULL h or text -> 0x80 (NULL h: written through NULL), FALSE.
2. UCS dialects: convert through the romanizer (FUN_1000423d).
3. Empty text -> TRUE, nothing else.
4. **Manual mode** (param 0 == 1): FUN_100071d6 queues an item {type 0, copy of the text, snapshot of the 17
   parameters, snapshot of the active voice}; with annotations on, the copy goes through the annotation parser
   (FUN_100083c1, 3.10) with target = the handle's *current* params/voice (so `` `vs `` etc. change what
   eciGetVoiceParam reports), no last-sent target, units = the snapshot's param 8. Out of memory -> 0x2, FALSE.
   TRUE. Nothing is sent.
5. **Normal mode**: FUN_10007309 (sends a leftover manual queue, normally empty), sendParameters, then
   - annotations off: `eciAddText2(text, strlen, 0, annot = 0, 0)`;
   - annotations on: calloc copy; FUN_100083c1(active voice, params, last-sent voice, last-sent params, units,
     copy) (updates both, so these values are not re-sent later, and may rewrite numbers); `eciAddText2(copy,
     strlen(copy), 0, 1, 0)`; free.
   Result 0 or -0xe -> TRUE (-0xe still sets 0x4000).

**eciAddText2** (FUN_100097b7 -> FUN_10015ced -> FUN_10015f3e): with annotations on, the text is scanned for a
backtick not preceded by `\` that starts `` `l<digits>[.<digits>[.<digits>]] `` (FUN_10015d36; value =
(a<<16) | (c<<8) | b for "`la.b.c"). The text before it is posted as a text command; a language command is
posted (FUN_1000af6b(0,2,...)); on success the annotation's characters are overwritten with spaces (they stay
in the next chunk). The rest (or the whole text) is one text command. Each **text command** (FUN_10011a8a):
first makes sure an output device is open (FUN_100119af, section 6), copies `len` bytes, adds len to pending,
+1 to the posted sequence, busy = 1.

On the worker, the text command (FUN_1001541d -> FUN_10012fce):
1. FUN_1000d93c: if the annotation flag differs from the previous text's, first flush the preprocessor's
   pending text; store the text; with a romanizer (languages 6/8/10) hand it over.
2. FUN_1000d7c8 (no romanizer): if not aborting, prepare the text (FUN_1000ddc5, 8.1) and return it once:
   FUN_10013080 -> `e.processSentences(copy)`; nonzero -> engine error (type 4, once). If a phoneme buffer is
   active, readPhonemes follows (5.4).
3. pending -= len.
There is **one processSentences per text command**, i.e. per eciAddText (per `` `l `` segment). No size limit
anywhere (copies are malloc'd to the length) [V].

FUN_10013080 also keeps the **character count for index placement** (controller+0x63, "chars"): chars += len +
(1 if the last byte is not a space) - (number of backslash escapes, counting `\x` pairs left to right). This
applies to every processSentences, annotations included.

**eciSynthesize(h)** (100047d6): guarded; NULL h writes to address 0x638 (crash). FUN_10007309 (manual
queue: 3.11); then `eciSynthesize2` = post command 0x7d5 carrying the new posted sequence number. Returns TRUE
if posted. **Does not call sendParameters** (a voice change after the last eciAddText is not sent). Worker side
(FUN_100138de):
```
pending -= 1
if pending == 0: FUN_10013571 -> (flush preprocessor text) e.processRemaining(NULL)
FUN_100139bf: if pending == 0:
    chars = 0; charsAtLastIndex = 0
    device mode (no audio error): FUN_10013a5d - wait for playback to end, close the device (6.1)
    buffer mode: FUN_10013fdf - deliver the partly filled buffer as eciWaveformBuffer (if > 0 samples)
    phoneme buffer: FUN_10013e03 - deliver the pending phonemes
FUN_1001394b: if pending == 0: post the type-7 end marker (data = this command's sequence number)
```
So the end of an utterance is: processRemaining(NULL), the last partial buffer, the marker (which ends
eciSynchronize). If more commands were posted before the worker reached the synthesize command, pending != 0
and **neither processRemaining nor the marker happens** for it (timing-dependent merge; also the posted
sequence moved on, so an earlier marker would not end eciSynchronize anyway) [V].

**eciSpeakText(text, annot)** (10003328) -> BOOL: no ECI handle, no window, no callbacks: FUN_10009354 creates a
temporary eci2 object for the default language, an audio format on device "0" (waveOut device 0) with 10/2200/0/2200
blocks and rate 0 (-> "`esr1"), retrying with 11025 then 8000 if that fails; eciAddText2(text, len, 0,
annot, 0); if OK or -0xe: eciSynthesize2 + eciSynchronize2 (blocks until spoken); deletes it. TRUE for 0 or -0xe.
No escaping differences (same text command path), no voice/param setup beyond the engine defaults.

**eciSpeakTextEx(text, annot, dialect)** (10003389): eciNewEx(dialect); annot -> eciSetParam(1,1); eciAddText;
eciSynthesize; eciSynchronize; eciDelete. On a failure after creation the handle is **leaked** (no eciDelete).

**eciTestPhrase(h)** (10003435): guarded; eciStop, eciCopyVoice(h, 1, 0), eciAddText("1 2 3.") (widened in UCS
dialects), eciSynthesize. BOOL.

**eciSynthesizeFile(h, filename)** (1000492e): guarded; NULL name -> FALSE. Opens the *text* file "rb" (fopen,
or _wfopen in UCS dialects); FUN_10007309; reads with fgets(512) (or fgetws(256)) and calls eciAddText for each
chunk (newlines included), stopping at the first failure; closes; FUN_10007309; eciSynthesize2; returns (r ==
0). An empty file -> TRUE without synthesizing. It does not write an output file.

### 3.5 Stop / clear / pause / state

**eciStop(h)** (10004f33): sets the reentrancy flag; frees the manual queue; eciStop2 = FUN_10012c0c (on the
calling thread):
```
if device: block its queue and wait for its current command; if playing: stop (waveOutReset..) and close;
           SetEvent(end-of-playback)
lock; e.flush(1) (nonzero -> -0xf); unlock
dispatcher: block, discard all queued callback items (waking blocked posters), drop the retained item,
            KillTimer, remove all messages of the window                          (FUN_100016d5)
preprocessor: abort flag (drops text not yet handed to the engine)
worker: block its queue, drop queued commands, wait until the running command finishes
e.flush(0)
clear: index-position list, user-index FIFO, partial sample count (the partial buffer is DISCARDED, not
       delivered), phoneme count, chars, charsAtLastIndex, pending, busy
unblock worker and device queues; preprocessor abort off; posted = completed = 0
dispatcher: unpause, PostMessage(hwnd,0x500,h,0), unblock
```
On success: 0x632 = 1 (everything is re-sent with the next text), TRUE. The index slot pool is not reset
(slots of indices that never fired stay allocated) [V].

**eciSpeaking(h)** (1000501a): sets the flag, eciPoll2 (delivers callbacks), clears the flag; TRUE for 3 or 1.

**eciSynchronize(h)** (10005106): sets the flag, eciSynchronize2, clears; result -2 or 1 -> FALSE (1 also sets
0x400 "paused"); otherwise TRUE. 4 also clears busy.

**eciSyncWait(h, ms)** (10005213): not guarded, h not checked. ms < 0: eciSynchronize. ms == 0: one eciPoll2,
returns (r == 3). ms > 0: busy loop (no sleep) of eciPoll2 until the result is not 3 or the time (ftime) is up;
**returns TRUE if it timed out while still speaking, FALSE when speech ended** [V].

**eciClearInput(h)** (10004b7c): guarded; frees the manual queue only. TRUE.

**eciPause(h, on)** (10005864): guarded; eciPause2 -> FUN_10012eb8: dispatcher pause flag = on; if on == 0 or
there is no retained item: (if one is retained, SetTimer) PostMessage(hwnd,0x500,h,0); else KillTimer. If a
device exists: audio command 0x3ea -> waveOutPause/waveOutRestart (-0x10 on failure). Returns TRUE if >= 0.
While paused, eciPoll2 returns 1/6 without delivering, eciSynchronize returns FALSE (0x400); in buffer mode the
worker stops at its next send-and-wait callback.

### 3.6 Output configuration -> section 6. eciRegisterCallback (10005918): guarded; stores cb and data; no engine call.

### 3.7 eciInsertIndex(h, index) (1000469d)

Guarded. Manual mode: queue {type 1, index, params, voice} (FUN_100072a9), TRUE. Normal mode: sendParameters;
`eciInsertIndex2` (FUN_10012062: open the device if needed; post command 0x7d2; pending += 1). TRUE if 0.

Worker side (FUN_100132ab), CallbackFlag without 0x200 (the shipped case):
```
slot = allocSlot(); slot = {kind 0 (user index), value = index}   ; no slot -> type-6 callback (memory)
if chars == 0:  e.insertSynthesisIndex(slot)
else:           append (slot, delta = chars - charsAtLastIndex) to the index-position list
charsAtLastIndex = chars
pending -= 1
```
The list is consumed by the engine's **word callback** (FUN_100152a9(count, user) -> FUN_10012a3b):
```
count = min(count, chars); chars -= count; charsAtLastIndex = max(0, charsAtLastIndex - count)
loop while the list is not empty:
    take = min(count, head.delta); head.delta -= take; count -= take
    if head.delta == 0: pop it, e.insertSynthesisIndex(slot) (called from inside the engine's callback)
    else if count == 0: stop
```
With CallbackFlag 0x200 instead: push the index on a FIFO and `e.processSentences("`ui")`; the engine's user
index callback (FUN_100152c0 -> FUN_10012b80) pops it, allocates a slot and calls insertSynthesisIndex.

Slots (controller+0x93, FUN_1000c4db/c589/c698/c8d6): 1-based numbers from a pool of 17-byte entries (8 bytes of
data); the pool starts with max(2*0, 1024/17) = 60 entries, numbers 1..60 on the free list in order, doubling
when empty (new numbers appended to the free list). Allocation takes the head of the free list; freeing
**pushes the slot back at the head** (LIFO reuse). The number is what the engine sees in insertSynthesisIndex /
insertDelayedSynthesisIndex and gives back to the index callback.

When the engine calls the index callback (FUN_10015c90(slot, user) -> FUN_100127ce):
- device mode (no audio error): audio command 0x3ee places the slot as a marker in the audio stream; it is
  handled when playback reaches it (FUN_1001532f from the audio thread).
- otherwise: **first** deliver the partly filled output buffer (FUN_10013fdf: eciWaveformBuffer with the
  current count, if > 0) and pending phonemes (FUN_10013e03), then handle the slot.
Handling a slot (FUN_10012848): kind 0 -> dispatcher item type 0 (eciIndexReply, lParam = value); kind 1 ->
look the phoneme up in the mouth table, type 3 if found; kind 2 -> SetEvent(end of playback); kind 3 -> type
8 (eciWordIndexReply); then free the slot. Type 0/3/8 items are posted without waiting.

Indices still in the list when an utterance ends stay there (chars is reset to 0, the list is not) **[?]**:
whether they fire depends on the word counts the engine reports.

### 3.8 Phonemes: eciGeneratePhonemes(h, size, buf) (10004c09), eciGeneratePinyins (10004c95)

eciGeneratePhonemes sets the reentrancy flag and runs FUN_10004d46(h, size, buf, 7). It requires a registered
callback and **manual mode** (param 0 == 1); otherwise FALSE **with the flag left set** (bug). Steps: tear down
the current output (mode 0 -> delete audio format; 1 -> unregister buffer); eciRegisterPhonemeBuffer2
(FUN_1001130e): refused while busy (-9), with a device (-10) or buffer (-0xb); size must be > 1;
`e.registerWordCallback(0)`, `e.processRemaining("`espr1")`, mirror+0x50 = 1, `e.registerWordCallback(FUN_100152a9)`;
buffer = buf, capacity size-1, temp buffer size*4+4; mode 2. Then FUN_10007309 (send the queue),
eciSynthesize2, eciSynchronize2 (callbacks delivered here), then restore the previous output (mode 0 ->
FUN_10006dd4, which first unregisters the phoneme buffer: esr-style group with "`espr0"; mode 1 -> re-register
the buffer; mode 4 -> none). Clears the flag, TRUE.

After every processSentences while a phoneme buffer is active (FUN_10013c0e): loop
`e.readPhonemes(temp+got, max-got, &n)`; each time the buffer is full, copy it to the user's buffer (strncpy,
or widened for UCS dialects) and deliver **eciPhonemeBuffer** (send-and-wait) with lParam = number of
characters. The rest is delivered at an index or at the end (FUN_10013e03). eciGeneratePinyins: same with
"`einp1"/"`einp0" (FUN_100115b9), only when the language is 6 (Chinese); otherwise returns 0 **with the flag
left set**.

### 3.9 Language switch (param 9 or `` `l ``)

Command 0x7d1, worker FUN_100135ec: `processRemaining(NULL)` first (FUN_10013571); if the language L differs:
switch to that engine object (loaded by FUN_10013429 when the command was posted; not in the INI -> -0xe),
register its callbacks (FUN_100131b2, including clearInput and flush(0)), set the mirror, unregister the word
callback, **replay every mirrored parameter** into the new engine through processRemaining (FUN_1000aefe ->
FUN_1000b2bd: "`ts%u", "`ty%u", "`da%u", wantPhonemeIndices(mirror[4]), "`vg%u".."`vv%u", "`pp%u"; then
"`espr1" if the phoneme-buffer mode is on), re-send "`esr" for the device format and for the output buffer,
re-register the word callback, and FUN_1000e372(2, L.D) (processRemaining(NULL) again, romanizer switch).
Same L, different D: record it and FUN_1000e372 (processRemaining(NULL)). Identical: nothing.

### 3.10 The annotation parser FUN_100083c1 (eciAddText with eciInputType 1)

Walks the text (read index i, write index w; the text is compacted in place and gaps refilled with spaces, so
its length never changes). At a backtick not preceded by `\`:
- `` `da<n> `` -> param 3 = n if 0..1; `` `pp<n> `` -> param 11; `` `ts<n> `` -> param 2 (0..3); `` `ty<n> `` ->
  param 10; `` `l<a>[.<b>[.<c>]] `` -> param 9 = (a<<16)|(c<<8)|b if a in 1..10, b in 0..1 and installed.
- `` `vg<n> `` -> gender if 0..1. `` `vh `vf `vr `vy `` -> value capped at the maximum; below the minimum it
  is ignored. `` `vb `vs `vv `` -> negative values are left alone; otherwise v' = (units ? inverse of the
  real-world value : v), capped at the maximum; v' is **written back into the text** in place of the number
  (if it has more digits than the original, v' becomes the original width of 9s), the rest of the number's
  width is filled with spaces.
- `` `v<n> `` (n 1..8, preset defined) -> the whole preset (including its name) becomes the target voice.
Values are written to the first target (params/voice) and, when given, the second (last-sent) target. Parsed
with sscanf("%i%n"), so hex/octal forms are accepted.

### 3.11 Manual mode at eciSynthesize (FUN_10007309)

If the queue is not empty: eciBlock2 (post command 0x7d4 that makes the worker wait on a semaphore until
eciUnblock2) so the whole utterance is queued before any of it runs. For each item: FUN_10007f6a(item params,
force = 0x632) and FUN_100082ec(item voice, force) (so parameter/voice annotations are sent per item, in item
order); text items with the item's eciInputType == 1 go through FUN_100083c1 once more to update only the
last-sent state (units 0), then eciAddText2(text, strlen, 0, annot, 0); index items -> eciInsertIndex2. The
first error frees the rest of the queue. Then eciUnblock2. Returns TRUE if everything was sent.

---------------------------------------------------------------------------------------------------------

## 4. (Summary) What reaches the engine for one plain utterance

Handle from eciNew, eciSetOutputBuffer(h, N, buf), optional eciCopyVoice(h, n, 0), eciAddText(h, "Hello."),
eciSynthesize(h), eciSynchronize(h) [V]:
```
eciSetOutputBuffer (caller thread): registerWordCallback(0); processRemaining("`esr1"); registerWordCallback(cb)
eciAddText         (worker):        processSentences("`vgN") ... one per voice parameter that differs, order g h b f r y s v
                                    processSentences(prepared text)
eciSynthesize      (worker):        processRemaining(NULL)
```
Audio comes back through the setSynthToCallback function during these calls (processSentences may already
produce audio for complete sentences). flush(1)/flush(0) appear only at eciStop/eciReset/eciDelete.

---------------------------------------------------------------------------------------------------------

## 5. Callback delivery

### 5.1 Item types -> user messages (FUN_100069ee) [V]

The user callback is called as `cb(hECI, msg, lParam, pData)`: four pushes, then `add esp,0x10` after the call
(cdecl-style caller cleanup; the frame pointer makes a stdcall callee harmless too). Only if a callback is
registered (h+0x10 != 0). Result: `cb() == 0` (eciDataNotProcessed) -> -1 (retry later); **any other value
(1 processed, 2 abort) -> processed**.

| item | sent from | user message | lParam | side effects |
|---|---|---|---|---|
| 0 | FUN_10012848 (index kind 0) | 2 eciIndexReply | the index value | h->0x3a8 = value (set even when the callback returns NotProcessed) |
| 1 | FUN_10013f27 / FUN_10013fdf, **send and wait** | 0 eciWaveformBuffer | data/2 = number of samples | |
| 2 | FUN_10013c0e / FUN_10013e03, send and wait | 1 eciPhonemeBuffer | number of phoneme characters | |
| 3 | FUN_10012848 (kind 1, found in the table) | 3 eciPhonemeIndexReply | pointer to h+0x5c4 (ECIMouthData) | filled first; NULL data -> error 0x10, no call |
| 4 | FUN_10015a6e | - | | error 0x10 |
| 5 | FUN_100158f9 | - | | error 0x20 |
| 6 | several (out of memory) | - | | error 0x2 |
| 7 | FUN_1001394b | - (end marker, handled in the poll) | | completed sequence = data |
| 8 | FUN_10012848 (kind 3) | 4 eciWordIndexReply | value | h->0x3a8 = value |
| 9 / 10 | FUN_10015c0c | - | | error 0x1000 / 0x4000 |

Error items (4, 5, 6, 9, 10) do not call the user callback, and are only created when a callback is registered.

### 5.2 Output buffer (eciSetOutputBuffer) [V]

The engine's audio callback is FUN_10015c76(count, int32 *samples, user) (cdecl) -> FUN_10012f07. With a
buffer registered (controller+0xc7) and for each sample in order (FUN_10013f27):
`buf[n++] = (int16)(low 16 bits of samples[i])` - **a plain truncation, not a clip** (machine code `mov dx,
word ptr [esi+edx*4]`). When 2n == the buffer size in bytes (= 2 x the eciSetOutputBuffer size): item 1 with
data = 2n (-> lParam = n = the full size), send and wait, then n = 0. The worker waits inside the engine's audio
callback until the user has processed the buffer; with NotProcessed the same buffer is offered again (30 ms
timer / next poll, or a busy retry inside eciSynchronize).

A partly filled buffer is delivered (lParam = the count so far, only if > 0):
- **before every index-type event** from the engine's index callback (user index, phoneme index, word index)
  - FUN_100127ce;
- at the end of an utterance (synthesize command with nothing else pending) - FUN_100139bf.
It is discarded by eciStop.

So the order the user sees is exactly the engine's order: full buffers as they fill, then at each index a
partial buffer followed by the index reply; at the end a final partial buffer; nothing marks the end except
eciSynchronize/eciSpeaking returning. With eciWantPhonemeIndices each phoneme index also cuts the audio into a
partial buffer.

### 5.3 The ECIMouthData for eciPhonemeIndexReply [V]

The engine's phoneme callback FUN_10015ca7(p1, p2, user) -> FUN_100129a0: slot {kind 1, value p1};
`e.insertDelayedSynthesisIndex(slot, p2)` (nonzero -> engine error). When the slot fires: binary search of the
current language's table (built from the INI keys `Phoneme0..`, sorted by the int made of the 4 phoneme bytes;
FUN_1000cf9b/FUN_1000d2c7) for p1. Found -> FUN_100069ee fills h+0x5c4:
- ANSI dialects: bytes 0x5c4..0x5c7 = the 4 phoneme bytes; UCS dialects: 4 wide chars at 0x5c4..0x5cb and
  flag 0x800 OR-ed into the dialect field;
- 0x5ce (int): the dialect of the INI section the entry came from;
- 0x5d2..0x5d9: low bytes of the 8 longs (mouthHeight, mouthWidth, mouthUpturn, jawOpen, teethUpperVisible,
  teethLowerVisible, tonguePosn, lipTension).
INI format: `PhonemeN=c0 c1 c2 c3 s0 s1 s2 l0 .. l7` ("%d %d %d %d %hd %hd %hd %ld x8"): the 4 phoneme bytes, 3
shorts (stored, unused here), 8 longs. What the engine passes as p1/p2 **[?]** (p1 is looked up as the 4-byte
phoneme code, p2 is passed on as the delay).

### 5.4 Word index replies

Registered only with CallbackFlag & 0x100 and eciWantWordIndex = 1 (FUN_100134ba): the engine's word index
callback FUN_10015cc1(value, user) -> FUN_10012b15: slot {kind 3, value}, `e.insertSynthesisIndex(slot)`.
Never happens with the shipped INI (0x3f).

---------------------------------------------------------------------------------------------------------

## 6. Output modes

**eciSetOutputBuffer(h, size, buf)** (100053a5): guarded; FALSE if h is NULL or **no callback is registered**.
- size == 0 or buf NULL: **param 5 (sample rate) = 1**; normal handle -> eciSetOutputDevice(h, -1) (back to the
  wave mapper); NewEx2 handle -> FUN_100070fc (no output) and the result is inverted (returns FALSE on success).
- otherwise: back up the output settings; h->0x398 = size, 0x3a0 = buf; FUN_10006fd5: tear down the current
  output (mode 0: delete the device object, no engine calls; mode 2/3: unregister the phoneme buffer) and
  eciRegisterSampleBuffer2(buf, 2*size bytes, {0, rate 8000|11025, 0}) = FUN_100110a4: -9 busy, -10 a device
  still exists, -0xc phoneme buffer active; bytes must be nonzero and even; format fields default to {2 (encoding id,
  meaning [?]), 11025, 16 bits} and must end up {2, 8000|11025, 16} (else -5); `registerWordCallback(0)`, `processRemaining(rate == 8000 ?
  "`esr0" : "`esr1")` (nonzero -> -0xf), `registerWordCallback(FUN_100152a9)`. Mode 1, last-sent rate
  updated, TRUE. On failure the backup is restored but the old output is already gone.

**eciSetOutputDevice(h, dev)** (10005722): guarded. Backup; name = sprintf("%d", dev); FUN_10006dd4(h, rate,
blocks...): tear down the current output (buffer: just forgotten; phoneme buffer: "`espr0"/"`einp0"), then
eciNewAudioFormat2({0, 8000|11025, 0, name, numBlocks, sizeBlocks, numPreroll, sizePreroll}) = FUN_10012538: -9
busy, -0xb buffer registered, -0xc phoneme buffer; zero fields are defaulted (name "0"); same format as the
current device -> nothing to do; otherwise release the old device and create a new one (FUN_1000ea57 -> audio
thread + command 0x3f1 choosing the driver: a numeric name (atoi != 0, or "0") -> the `dev` driver with waveOut
device id atoi(name), -1 = WAVE_MAPPER; otherwise a file driver chosen by the text after the last '.', case
insensitive: wav, au, snd, ral (raw A-law), rau (raw mu-law); unknown -> wav, the first registered); failure ->
-4 (0x20). Then the esr group (wordCallback 0, "`esr0|1", wordCallback). Mode 0; last-sent rate and block params
updated; TRUE. **Nothing is opened yet.**

**eciSetOutputFilename(h, name)** (10005538): guarded. NULL -> like eciSetOutputBuffer(NULL) (rate param = 1,
device -1; NewEx2 inverted result). Otherwise strcpy into the 256-byte h+0x3c4 (**no length check**) and
FUN_10006dd4 -> file driver by extension.

### 6.1 Device/file mode at run time [V, driver internals partly]

- Before posting a text/annotation/index command, FUN_100119af: query the device state (command 0x3ef); if
  closed, open it (0x3e8 -> FUN_10017f40: files with fopen "r+b", else "wb", then fseek(0) - **each utterance
  starts writing at offset 0 again** [?: the file drivers' open/close functions 0x10018130-0x1001a590 are only
  reachable through the driver tables, Ghidra did not decompile them]); open failure 3 -> -0xd (0x200
  "busy"), otherwise -2; then start the 30 ms position ticker (0x3f0).
- Audio callback (FUN_10012f07): unless an audio error happened, FUN_1000fa96 sends the samples to the audio
  thread (command 0x3e9, at most 8000 samples per command, larger counts in 2000-sample pieces), retrying every
  250 ms while the driver reports "full/paused" (2). The `dev` driver (FUN_1001a910/FUN_1001b110) converts to
  its format (16-bit: truncation to the low 16 bits when the ranges match), a 20000-byte ring of 10 WAVEHDRs x
  2000 bytes, and holds the first 8 blocks back as preroll before the first waveOutWrite. It opens the wave
  device (PCM mono, rate from the format, 16 bit) with waveOutOpen(..., CALLBACK_NULL). The numDeviceBlocks /
  sizeDeviceBlocks parameters are carried in the format but the `dev` driver uses fixed 10 x 2000 bytes **[?]**.
- Indices: placed as markers in the audio stream (0x3ee); the ticker's position polls fire them when playback
  reaches them, from the audio thread -> FUN_10012848 -> dispatcher.
- End of utterance (FUN_10013a5d): slot {kind 2}, marker, one zero sample, flush the partial block (0x3ed,
  retry every 250 ms), then the worker **waits until playback reaches the marker**, then closes the device/file
  (0x3eb). Only then is the end marker posted, so eciSynchronize returns after the sound has played.
- Audio errors: item 5 (0x20), device closed, further audio dropped until eciClearErrors.

**No buffer and no device** (eciNewEx2 without eciSetOutput*, mode 4): the audio callback does nothing (both
paths are skipped); indices are delivered as in buffer mode (without the partial-buffer step); synthesis runs
as fast as the engine does.

---------------------------------------------------------------------------------------------------------

## 7. Dictionaries

ECIDictHand = pointer to a 0x14-byte block {dialect, engine object, engine dict handle, romanizer, romanizer
dict} (FUN_10014070). Return codes via FUN_10008d41: -2 -> 2 (DictOutOfMemory), other negative -> 6
(DictAccessError), >= 0 -> 0. None of these functions is guarded or checks h for NULL (except where noted).

| export | behaviour | engine call |
|---|---|---|
| eciNewDict(h) (100059a2) | NULL h -> NULL. Busy -> error 0x2000, NULL. Else **sendParameters, eciSynthesize2, eciSynchronize2** (so it ends the current input and delivers callbacks!), then for the current dialect: newDict; romanizer dict too for 6/8/10 | 27 newDict() |
| eciGetDict(h) (10005a89) | the dict last set for the current dialect's engine (holder[5]) or NULL | none |
| eciSetDict(h, d) (10005b11) | NULL h -> 6. d NULL: for every L 1..10, D 0..1 with a recorded dict: deactivate (setDict(0)); clear the table; 0. d: activate, record it at h+0x5da+L*8+D*4 | 29 setDict(engineDict) / setDict(0) |
| eciDeleteDict(h, d) (10005ba7) | forget it in the table if recorded; if it is the active one holder[5] = 0; deleteDict; free the block. Returns NULL | 30 deleteDict(engineDict) |
| eciLoadDict(h, d, vol, file) (10005c9b) | NULL file -> 6. vol 0..2: loadDict; vol 3: romanizer | 31 loadDict(engineDict, vol, file) |
| eciSaveDict(h, d, vol, file) (10005da4) | same | 32 saveDict(engineDict, vol, file) |
| eciUpdateDict(h, d, vol, key, value) (10005ead) | value: leading spaces skipped; key must be non-empty; value NULL -> passes NULL (delete); value "" -> -3 -> 6; copies; codeset 10 for Korean (key/value converted by the romanizer) | 33 updateDict(engineDict, vol, key, value) |
| eciDictFindFirst(h, d, vol, &key, &value) (10005f9e) | engine returns two char*; NULL -> 4 (DictNoEntry). Copies are kept in the controller (valid until the next call); *key/*value = the copies; 0 | 34 dictFindFirst(engineDict, vol, &k, &v) |
| eciDictFindNext (100060b7) | same | 35 dictFindNext(...) |
| eciDictLookup(h, d, vol, key) (100061d0) | returns a copy of the translation or NULL | 36 dictLookup(engineDict, vol, key) |
| eciUpdateDictEx, eciDictFindFirstEx, eciDictFindNextEx, eciDictLookupEx (100062f6..10006734) | only for vol 3 with a romanizer (Chinese/Japanese/Korean); otherwise 6 (FindFirstEx/NextEx: 4) | romanizer only |

---------------------------------------------------------------------------------------------------------

## 8. Surprises, quirks, bugs

1. **Text preparation** (FUN_1000ddc5, on the worker, per text command) [V; the escape strings read from
   0x1001d4b0 = `\`, 0x1001d4b4 = `\\`, 0x1001d4b8 = `\\\\`]:
   - Each byte first goes through the table at 0x1001d3b0 **in place**: 0x00-0x09, 0x0b-0x1f, 0x7f, 0x81-0x84,
     0x86-0x89, 0x8b, 0x8d-0x90, 0x9b, 0x9d-0x9f, 0xff -> space; 0x91, 0x92 -> `'`; everything else unchanged.
   - `\n`: becomes a space, unless it is not the first byte and the byte before it is a space, in which case it is
     dropped.
   - `|` -> `\| ` (backslash, bar, space); `^` -> `\\^ ` (2 backslashes, caret, space); with annotations off,
     `` ` `` -> ``\` `` + space and `\` -> `\\\\` (4 backslashes).
   - With annotations on: a backtick followed by `g`, `i` or starting `` `ui `` -> ``\` `` + space (escaped);
     other backticks pass. A `\` followed by `\` or `` ` `` is dropped and the next byte is escaped as above
     (`\\` -> `\\\\`, ``\` `` -> ``\` `` + space); any other `\` -> `\\\\`.
2. **eciDataAbort is not implemented** (treated as processed).
3. **Samples are truncated to 16 bits, not clipped**, in ECI.DLL (FUN_10013f27, and the `dev` driver). The
   traced "clip" must come from the engine side or be a coincidence **[?]**: check with a very loud voice.
4. The default ECI parameter values and the default active voice are read back from the engine (annotation
   callback) after the init string, not hard-coded (1.3).
5. Every ECI handle loads its own engine object; the voice presets and phoneme tables are global.
6. One "class registered" flag for two class names: eciNewEx2 first -> later eciNew windows fail (class
   "eciWindowETI" never registered, CreateWindow fails -> eciNew returns NULL); the pump thread always uses
   "eciWindowETI" and then fails too.
7. Reentrancy flag left set: eciReset on an eciNewEx2 handle (also returns FALSE); eciGeneratePhonemes without a
   callback or outside manual mode; eciGeneratePinyins on non-Chinese.
8. eciCopyVoice does not validate `to` when `from` is 1..16 (writes before the user voices).
9. eciSetOutputBuffer/Filename(NULL) set eciSampleRate to 1, and on NewEx2 handles return the inverted result.
10. NULL-handle crashes: eciSynthesize (writes to 0x638/0x634), eciAddText/eciGetVoiceName (write through NULL),
    eciErrorMessage/eciSetVoiceName (UCS path), eciSyncWait.
11. eciSetParam returns -1 for the device-block parameters 13..16 (they can't be changed through ECI).
12. eciSetOutputBuffer requires a registered callback.
13. eciNewDict synthesizes whatever is pending and waits for it.
14. A textMode change to nonzero, eciWantWordIndex = 1, a wantPhonemeIndices change, and every language command
    each insert `processRemaining(NULL)` (an utterance break) into the engine stream.
15. eciStop discards the partly filled output buffer; eciClearInput does not touch text already given to the
    engine; the index slot pool is never reset.
16. With eciRealWorldUnits and annotations, `` `vb `vs `vv `` numbers are rewritten in the text sent to the
    engine (3.10).

---------------------------------------------------------------------------------------------------------

## 9. Open questions (worth a trace)

1. The engine annotation callback: which (id, value) pairs it reports after "`v1 `ts0 `da1 `ty1 `pp1", i.e. the
   real defaults of params 2/3/10/11 and of voice 0.
2. Word callback semantics: does the engine's count include annotation characters and the +1 per chunk that
   FUN_10013080 adds? This decides where queued indices are inserted, and hence where partial buffers are cut.
3. The clip-vs-truncate question (8.3).
4. Phoneme callback arguments (p1 phoneme code, p2 delay) and the phoneme table lookup key.
5. The file drivers (wav/au/snd/ral/rau) and `dev` driver entry points at 0x10018130-0x1001a590 are not
   decompiled (only referenced from the tables at 0x100242f8..0x100244a8); header formats and whether a second
   utterance overwrites the first are unverified.
6. eciSynchronize ignoring a retained (NotProcessed) item left by an earlier eciPoll2 (FUN_1000128f).
7. Whether eciNew fails on a machine without a wave device (the `dev` driver's configure function is one of the
   undecompiled ones).
