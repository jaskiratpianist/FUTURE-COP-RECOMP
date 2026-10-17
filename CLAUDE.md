# CLAUDE.md — Future Cop static recompilation

Read this first. It is the hand-over from the sessions that built this project (they
worked in an earlier, combined repository that also held a matching decompilation and an
old PE loader; only the recompiler moved here).

## Goal (the user's)

A modern native port of Future Cop: L.A.P.D. (1998). Wanted, in order:

1. The game playable natively (Linux first) from recompiled code.
2. A **native quit option** and a **preferences menu**: control remapping (input is
   currently unreliable), graphics settings, resolution.
3. **High resolution** through the game's 3dfx Glide path reimplemented on OpenGL, then
   **remastered assets** (textures by hash replacement, possibly the player robot model).
4. Later: Windows and **Android** builds.

Only the code is ever shared; players supply their own game files. **Never commit
anything derived from the EXE** (generated C, extracted data). `game_files/` and `build/`
are git-ignored. Rust was discussed and explicitly put off: stay in C.

## The user

- New to reverse engineering. Explain in plain words, and give a recommendation rather than a survey.
- Works in short sessions and watches usage meters (Claude session/weekly limits, shares
  screenshots). When unattended, keep **Claude usage under about 70% of the session**,
  and pause near the limit. Stop immediately when told to stop or finish.
- Has an Antigravity subscription. Its CLI `agy` runs **Gemini** (use freely; Gemini
  3.8 Flash works best, Gemini 4 Argon is slower and no better). Antigravity's
  Claude/GPT pool is small and was used up once. Use Gemini for bulk, well-specified
  work; keep Claude for design, review and debugging. Gemini makes careless mistakes
  (empty stubs, wrong struct offsets): review its output.
- Confirm before anything outward-facing or destructive (pushing, deleting). The user
  edits agy's permission file (`~/.gemini/antigravity-cli/settings.json`) themselves;
  never change it yourself.

## Layout

```
recomp/x86c.py        x86 -> C translator (writes build/recomp/gen/: gen_*.c, protos.h, table.c, imports.c)
recomp/rt/cpu.h       guest CPU struct, memory access, flags/x87 macros
recomp/rt/rt.c        dispatch (address -> function), x87 helpers, cpuid/rdtsc/in/out
recomp/rt/harness.c   test harness for difftest (demand-paged memory, traces)
recomp/difftest.py    Unicorn vs recompiled C differential tests
recomp/platform/      Win32/DirectX on SDL2: core.c (memory, thunks, handles, COM, loader, main),
                      host.h (rules for import code), win.h (shared window/input state),
                      k32_*.c u32_*.c gdi_misc.c ddraw.c dinput.c dsound.c
recomp/build.sh       generate + compile + link -> build/recomp/fcop
recomp/plat_test.py   build and run the game briefly, print the log tail (the one command Gemini may run)
recomp/plat_agy.py    hand one platform file to a Gemini worker (assignments are in this file)
recomp/paths.py       finds FCopLAPD.exe under game_files/ (or $FCOP_GAME_DIR)
data/functions.json   function boundaries (Ghidra headless export + gap filling + fragment merging,
                      produced in the old repo; treat as fixed input)
data/symbols.txt      IAT addresses of the 200 imports
docs/RECOMP_PLAN.md   design and milestones; docs/PLATFORM_TASK.md is the Gemini brief
```

Setup: `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`, SDL2 dev
package installed. Build: `./recomp/build.sh`; run: `build/recomp/fcop <game dir>`.

## How the recompiled code works

- Guest memory is one 4 GB reservation; guest address `a` is `MEM + a`. The EXE's sections
  are loaded at their original addresses (image 0x400000-0x522000), the main stack is
  0x100000-0x400000, the TIB at 0x10000, the guest heap at 0x01000000-0x40000000, and import/
  COM thunks at 0x7f000000. Game pointers stay 32-bit on any host (64-bit and ARM ready).
- Each original function becomes `void F_<addr>(Cpu *c)`. Registers and flags live in C
  locals (LOAD/SYNC around calls), so gcc removes unused flag computations. Flags pass
  through `c->fl` only on jumps between functions (FSYNC).
- `call` pushes the real return address on the guest stack and calls the C function, so
  `ret N` and stack-relative argument access work unchanged.
- Indirect calls and jumps go through `dispatch()` (a sorted table). Addresses that aren't
  recompiled functions go to `rt_foreign()` in core.c: import thunks run host code, then
  pop the arguments for stdcall.
- Extra entry points are generated for every place other code jumps or calls into the
  middle of, every code address stored in the data sections, and code that runs past a
  function's end. Switch tables become `switch`, with a shared per-function dynamic
  jump fallback (`dyn:`) for anything else.
- x87 uses doubles (the CRT sets 53-bit precision), with an exact s64 shadow so the FPU
  memcpy idiom (fild/fistp qword) is lossless.
- **Guest threads take turns** (core.c `turn_take`/`turn_give`): only one thread runs guest
  code at a time, handing over at every import call and, when another thread waits, at loop
  heads (`POLL` after every backward-jump label). The game has races that only show up when
  its threads truly run in parallel (the async file reader at 0x415f30 and its callback
  0x416bf0 share a one-slot mailbox; the main thread spin-waits on 0x4a03d8).
- **Self-modifying code**: the software renderer (around 0x45b000-0x461000) patches
  immediates, displacements and jump targets in its own inner loops, and
  FUN_0045b1c0 rewrites code for the 555/565 pixel formats from a table at 0x48ea20.
  x86c finds the written fields and makes the generated code read them from guest
  memory; rewritten opcodes get one version per encoding, chosen by comparing the
  current bytes.

## Debugging tools

- `recomp/difftest.py [addrs] [-v]`: differential test (whole EXE in about 10 s on 20 cores).
  `recomp/difftest.py <addr> --trace N` prints the first instruction where Unicorn and the
  C disagree (builds a traced library with `-DRECOMP_TRACE`). Known, expected failures:
  0x45b1c0 (cpuid), 0x4742e0 (rdtsc), 0x47c070 (FPU exception flags), 0x4764d0 (`in`
  on garbage input), 0x4532f0 (page-count difference), 0x45b6db (`fist` at 0x45bb30 on
  random FPU values; not yet checked whether it matters).
- Environment switches for the game: `FCOP_TRACE_API=1` (every import call with its
  caller), `FCOP_TRACE_MSG=1` (window messages), `FCOP_DD_LOG=1` (DirectDraw blits),
  `FCOP_SHOT=build/shots/f%04d.ppm` (dump every presented frame; convert to PNG to look;
  `FCOP_SHOT_LOG=1` logs frame numbers so they line up with other log lines),
  `FCOP_TRACE_KEYS=1` (each key code GetAsyncKeyState is asked about, and every press seen),
  `FCOP_INPUT="9000:click@484,303 28000:Keypad_8*3000"` (scripted input, ms after start: a
  click at game coordinates, preceded by a mouse move, or an SDL key name held 100 ms or *N ms).
  That example starts the first mission from the main menu and walks forward.
- Static facts: `data/functions.json` plus capstone. The generated C has the original
  instruction as a comment on every line (`/* 45b7c6: jmp 0x45ba37 */`), so
  `grep "TR(0x45b7c6u)" build/recomp/gen/*.c` finds any instruction.

## Gemini workers

`recomp/plat_agy.py <group>` sends `docs/PLATFORM_TASK.md`, plus the group's assignment
from GROUPS in that file, to `agy -p`. Each worker owns one file. `plat_test.py` takes
a lock so workers' builds take turns, and build.sh keeps a file's last good object when
it fails to compile. agy needs these allow rules in its settings, with this repository's
path (the user adds them):
`command(.venv/bin/python -I recomp/plat_test.py)` and
`write_file(<repo>/recomp/platform)`, plus the repository under `trustedWorkspaces`.
Workers quit when a blocked tool is denied (a shell `cat`, a URL fetch); just restart them.

## Lessons from the platform work

- Gemini left `SystemParametersInfoA` as an empty stub. The game reads SPI_GETWORKAREA to
  clamp its window origin, and blitted everything to y=26 million: always check stubs.
- The window is the whole screen in this port: `ClientToScreen` is the identity (the game
  adds the client origin to every blit into the primary surface in windowed mode).
- The CD check wants volume label `FCOP_CD` on a CDROM drive D:, and paths like
  `D:\FCopLAPD\Missions\...`; `host_path()` maps a leading `FCopLAPD` folder to the game directory.
- SDL turns SIGTERM into SDL_QUIT, so killing the game with `timeout` makes it quit cleanly
  (ExitProcess in the log is not a crash).
- Input: the menu is mouse-only (keys go to DefWindowProc) and picks the button under the last
  WM_MOUSEMOVE, so a click without a move before it does nothing. In missions the game polls
  GetAsyncKeyState; the default controls are the numeric keypad (8 forward, etc.), plus Ctrl,
  Alt, Space, Tab, Z, M, Esc, F1-F6, F10 (bindings come from the registry). DirectInput is only
  used to enumerate joysticks.
- COM objects share one vtable per interface (core.c `com_vtable`): the game creates surfaces
  and sound buffers constantly and thunks are never freed.
- `neg ecx; jmp [ecx*4 + T]` (memcpy's tail) reads up to 7 entries *before* T; x86c handles it.
- Intro movies use DirectShow (`CoCreateInstance` CLSID_FilterGraph); we return
  E_NOINTERFACE and the game waits a few seconds, then skips them.

## Next steps

1. Done: the menu works with the mouse and the first mission starts, renders and responds
   to the keypad. Next: play longer (FCOP_INPUT scripts), check other missions and the
   in-game Esc menu, map arrow keys/WASD as extra defaults later (preferences menu).
2. Fix in-game crashes and rendering (expect more stub/offset bugs in Gemini's code;
   review ddraw.c, dsound.c, dinput.c against the DirectX 5 SDK layouts).
3. Sound through dsound.c and SDL audio; music streaming uses the play/write cursors.
4. Glide: `LoadLibraryA("glide2x.dll")` currently returns 0 (software renderer). Implement
   the gr* functions the EXE names (strings like `_grDrawTriangle@12`) on OpenGL, return
   them from GetProcAddress, and render at any resolution.
5. The preferences/quit menu and control remapping, as new host-side code.
