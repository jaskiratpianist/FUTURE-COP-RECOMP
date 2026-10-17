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

`recomp/plat_agy.py --task docs/X.md` sends any brief (DIALOG_TASK.md is a good model:
exact spec, the test command with `--input`/`--png` so Gemini can look at its own screenshots).
`recomp/plat_agy.py <group>` sends `docs/PLATFORM_TASK.md`, plus the group's assignment
from GROUPS in that file, to `agy -p`. Each worker owns one file. `plat_test.py` takes
a lock so workers' builds take turns, and build.sh keeps a file's last good object when
it fails to compile. agy needs these allow rules in its settings, with this repository's
path (the user adds them):
`command(.venv/bin/python -I recomp/plat_test.py)` and
`write_file(<repo>/recomp/platform)`, plus the repository under `trustedWorkspaces`.
Workers quit when a blocked tool is denied (a shell `cat`, a URL fetch, reading a file outside
the repository such as SDL headers: put the API facts they need in the brief); just restart
them. A worker's session also ends when it waits for a test it started as a background task,
so briefs say "run the test in the foreground", and long tasks should append findings to a
file as they go. The whole reply is saved in `build/agy_<task>.out`. Two workers can run at
once on different files (plat_test.py serialises the builds). Review their claims with your
own test run (`plat_test.py` is headless; `--png`, `--audio`, `--input`).

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
- Dialogs: Preferences, Controller, Audio, the Esc pause bar, Load/Save and the quit box are
  the game's own RT_DIALOG resources driven by its dialog procedures. dialog.c (written by
  Gemini from docs/DIALOG_TASK.md, reviewed) parses the templates, runs them modally over the
  last game frame and draws them in the game's blue style; u32_win.c hands dialog hwnds to it
  (dialog.h). `FCOP_DLG_LOG=1` logs every dialog message. The game ignores IDCANCEL in most
  dialogs, so Esc presses a "Cancel"/"Resume..." button. `build/dialogs.txt` (made by a
  throwaway decoder, not committed) lists every control.
- Settings live in our registry (`registry.txt` in the game folder) under
  HKLM\Software\Electronic Arts\Future Cop\Settings (Resolution = list index, FullScreen,
  volumes...). The installer used to create the key; gdi_misc.c creates it now. The game saves
  them **when it exits normally** (plat_test's kill skips that: quit with
  `click@156,306` + `Return` in the input script). `FCOP_TRACE_REG=1` logs registry calls.
- Display: menus are always 640x480; the chosen resolution and fullscreen apply in missions.
  Windowed, DirectDraw's primary surface is the whole desktop (we report DESKTOP_W x DESKTOP_H,
  1600x1200, win.h) and only the main window's rectangle is shown. Fullscreen comes from
  SetCooperativeLevel (SDL fullscreen-desktop, letterboxed); Alt+Enter toggles it.
- Intro movies use DirectShow (`CoCreateInstance` CLSID_FilterGraph); we return
  E_NOINTERFACE and the game waits a few seconds, then skips them.

## Next steps

1. Done: menus, missions in both modes (incl. 2-player split screen), sound and music,
   dialogs, settings, resolutions/fullscreen, arrow keys, key rebinding, gamepads (via SDL,
   tested with the virtual pad only). Next: play longer (FCOP_INPUT scripts), check other missions and the
   in-game Esc menu, map arrow keys/WASD as extra defaults later (preferences menu).
2. Fix in-game crashes and rendering (expect more stub/offset bugs in Gemini's code;
   review ddraw.c, dsound.c, dinput.c against the DirectX 5 SDK layouts).
3. Sound through dsound.c and SDL audio; music streaming uses the play/write cursors.
4. Hardware 3D / high resolution. Findings (2026-10-17): the game draws 3D through EA's
   **THRASH** driver interface: 34 `_THRASH_*` functions in a driver DLL named by the
   registry value "Thrash Driver". The CD has them in `game_files/setup/thrash/` (voodooa,
   voodoo2a (imports glide3x), d3da (Oct 1998, newest), **opengla** (THRASH on OpenGL,
   loads opengl32 dynamically), softtria/softlina (software), ...), all based at 0x60000000
   (free in our guest layout) with relocations. The installer copies one into the game
   folder; ours has none and the game reads only `SoftwareRendering`, so it uses its built-in
   software renderer and never calls LoadLibraryA for a driver; find what makes it choose
   hardware (the `Group` value, display caps). The EXE also names the whole Glide 2 API.
   Two routes: (A) recompile a driver DLL with x86c (needs multi-image support and function
   discovery for DLLs) and forward opengl32 calls to host GL, scaling the viewport for any
   resolution; (B) implement the 34 THRASH functions natively on OpenGL, using opengla.dll
   as the reference for their semantics. B gives the cleanest high-res and texture
   replacement path; decide with the user.
5. Dialogs work (pause/resume, quit prompt, preferences). Still to check: Controller
   Configuration key capture, Audio sliders (once dsound plays), Load/Save. Then native extras
   (resolution, scaling, fullscreen, arrow/WASD keys) added to the Preferences dialog, and
   the game's own menu font instead of font8x8.
