# Eloquence 6.1 port - working notes for Claude

Bit-exact C port of ETI Eloquence 6.1.0.2 (ECI.DLL + ENU.SYN, US English) with the engine's data compiled in.
The goal is the same samples as the real engine, not "sounds right". README.md is for users; notes/port.md is
the authoritative engineering log (read its sections before touching a subsystem, and add to it when you learn
something); notes/eci_api.md specifies the ECI API.

## Rules that are not negotiable

- **Never commit vendor material.** pkg/ (ECI.DLL, ENU.SYN, ECI.INI), src/gen/ (generated from ENU.SYN: the
  recompiled engine, the lifted rules, the embedded data, prologues.c), decomp/ and Ghidra projects stay local
  (.gitignore covers them). Tools that *generate* from the user's copy are fine to commit.
- **Everything is verified against the real engine.** A change is done when the regression is identical, not
  when it compiles. Report results as counts ("195/195 lines identical"), and say plainly what was not run.
- **Be honest about what is hand-written.** The user wants real hand-written C. Say which parts are
  hand-written (klatt.c, framer.c, rules*.c, tracks.c, eci*.c), which are lifted by a tool
  (src/gen/rules_lifted_*.c, tools/delta_lift.py) and which are still recompiled (tools/x2c.py).
- The original's quirks are load-bearing: uninitialized stack bytes, x87 80-bit intermediates, msvcrt's x87
  log/exp/pow. Reproduce them, never "fix" them.
- Commit only when asked; the repo is public (github.com/KamiKitsune420/eloquence-decomp). No emoji support
  (user declined).

## Building (PowerShell only)

Git Bash does not get the MSVC environment (`string.h` not found) - run builds from PowerShell.

```
$env:ELOQ_MP="2"                               # 2 compilers at a time; 4 runs this laptop out of RAM
python tools/build_engine.py x64               # eloq_run.exe, ECI.DLL, ecitrace.exe  (also: x86)
python tools/build_engine.py x64 difftest      # build/x64/difftest.exe (write-tracking build)
```

The generated files are ~1 GB each to compile; a first build takes tens of minutes, later ones only
recompile what changed. difftest.exe is locked while tools/difftest.py runs (the link fails).

Regenerating (only after changing a generator or ported.h):
`python tools/delta_lift.py`, then x2c with `--replace-list src/ported.h --replace-list src/gen/lifted.h`
(full command in README.md), `python tools/prologues.py`. x2c rewrites only the files that changed.

## Checking

```
python tools/regress.py -j 4                   # e2e vs ecisay.exe (8 voices, hard, escapes, long) + ECI API scripts x64/x86
python tools/difftest.py -j 4 [--quick] [--only addr,addr]   # every ported/lifted function vs its recompiled original
```

difftest switches (details in notes/port.md): `DIFFTEST_RTRECOMP=1` (hand ports run as their originals - the
check for lifted rules), `DIFFTEST_SCRATCH=1` (also compare the stack scratch below each function's entry esp),
`DIFFTEST_WATCH=addr`, `DIFFTEST_WATCH_RS=1`, `DIFFTEST_CALLLOG=1` (trace who writes a byte / where two runs
part), `DIFFTEST_SHOW=n` (list n differing bytes per report). Useful listings: `python tools/disasm.py 0xSTART 0xEND`, `python tools/pushes.py ADDR` (stack traffic).

Last verified state (2026-09-25 evening): regress identical x64 + x86 (195/195 lines, 23/23 API scripts);
full DIFFTEST_SCRATCH run: 0 mismatches in every hand port over 203M calls; 156 015 remain in 41 lifted
rules, all stack scratch (the lifter does not reproduce every dead stack byte; nothing reads them).

## How the code fits together

- `src/x86rt.*`, `src/fx80.c`: the software x86 and exact x87 the recompiled code runs on (guest memory,
  registers in `cpu *c`).
- `src/ported.h`: every engine function replaced by a hand port (`PORTED(addr, flags)`); x2c renames the
  recompiled one to `f_ADDR_recomp`. Ports are written with `PORT_FN(addr)` (src/port.h), which first runs
  `port_prologue` (the original's register saves, generated). `src/gen/lifted.h` lists the lifted rules.
- A port works in guest memory at the original's addresses and calls other engine functions *through the
  machine* (`call2/call3/call_at` in rules_int.h) where the original calls them, with the machine's registers
  holding the original's values around the call (callees save them on the stack), restored before return.
- Lifted rules (`LIFTED_FN`, src/lift.h) keep their frame in guest memory and mirror the original's registers
  at every call and return.

## Current work (see notes/port.md for the full state)

1. **Hand ports' scratch made exact**: done - every hand port leaves the original's stack bytes (recipe and
   the synthesizer/frame builder details in notes/port.md). Any new or changed hand port must keep
   `DIFFTEST_SCRATCH=1` clean.
2. **The whole engine hand-written** (user's decision 2026-09-25 evening): no recompiled or lifted code may
   remain - the 993 lifted rules (~225k Ghidra lines) and ~310 still-recompiled functions (list:
   build/remaining_real.txt; indirect-only targets: `ELOQ_ICALLS=file` logs them) all become hand-written C.
   Exactness contract chosen by the user: **output-exact, clean C** - audio and ECI API identical, engine
   memory identical except bytes the original leaves uninitialized (masked in the tester; any that reach the
   output are reproduced deliberately and documented). No return addresses / register echoes in new code;
   at the end the software x86 and all generated code are deleted.

## Environment gotchas

- The laptop has ~15 GB RAM; long background builds have been killed for low memory. Keep ELOQ_MP=2 and do
  not run two builds at once.
- The 32-bit reference tools (build/ecisay.exe, harness/) drive the real DLL; the 32-bit process is
  UAC-virtualized under C:\Windows, the 64-bit port is not.
- tools/disasm.py takes 0x-prefixed addresses; `--back N` shows what leads to an address.
