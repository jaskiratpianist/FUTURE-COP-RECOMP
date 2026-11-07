# CLAUDE.md — Future Cop static recompilation

Read this first. It is the hand-over from the sessions that built this project (they
worked in an earlier, combined repository that also held a matching decompilation and an
old PE loader; only the recompiler moved here).

## Goal (the user's)

A modern native port of Future Cop: L.A.P.D. (1998). Wanted, in order:

1. The game playable natively (Linux first) from recompiled code.
2. A **native quit option** and a **preferences menu**: control remapping (input is
   currently unreliable), graphics settings, resolution.
3. ~~High resolution through a hardware renderer, remastered assets~~: **both dropped by the
   user** (2026-10-18/19). The original software renderer is the target: it already does
   every resolution up to 1280x960. Tried and abandoned: an addon texture loader (reverted,
   commits ae8d924/316beec) and the game's 3dfx Glide path on OpenGL (branch `opengl`: it
   worked but broke some textures and effects). Don't revive them unless asked.
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
                      setup.c (game_files/ and user_info/ next to the executable; first-start
                      install from the CD image with its own ISO 9660/Joliet reader),
                      host.h (rules for import code), win.h (shared window/input state),
                      k32_*.c u32_*.c gdi_misc.c ddraw.c dinput.c dsound.c
recomp/build.sh       SDL2 (downloaded once, static) + generate + compile + link -> ./fcop
recomp/plat_test.py   build and run the game briefly, print the log tail (the one command Gemini may run)
recomp/plat_agy.py    hand one platform file to a Gemini worker (assignments are in this file)
recomp/paths.py       finds FCopLAPD.exe under game_files/ (or $FCOP_GAME_DIR)
data/functions.json   function boundaries (Ghidra headless export + gap filling + fragment merging,
                      produced in the old repo; treat as fixed input)
data/symbols.txt      IAT addresses of the 200 imports
docs/RECOMP_PLAN.md   design and milestones; docs/PLATFORM_TASK.md is the Gemini brief
```

Setup: `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`, SDL2 dev
package and cmake installed. Build: `./recomp/build.sh` (`DEBUG=1` for gdb, `SYSTEM_SDL=1` to
skip the static SDL2); run: `./fcop`. The executable is self-contained (only libc): it finds
`game_files/` and `user_info/` next to itself (SDL_GetBasePath); a folder given as argv[1] or
`FCOP_GAME_DIR` overrides game_files (plat_test.py does that), an image file as argv[1] installs it.

Windows: `TARGET=windows ./recomp/build.sh` cross-compiles `fcop.exe` (64-bit, static: only
Windows' own DLLs, UCRT included in Windows 10+). It downloads llvm-mingw (clang + MinGW-w64,
pinned version and sha256) into build/llvm-mingw once and builds SDL2 from the same source tarball
into build/sdl2-win; objects go to build/recomp/obj_win. Everything OS-specific that needs
windows.h lives in platform/os.c (`os_reserve`/`os_commit` for guest memory: the heap is committed in
16 MB steps as galloc grows it, because Windows charges committed memory to the page file;
`os_pick_image` = GetOpenFileName, `os_open_folder`, `os_mkdir`, `os_log_init`: without a console the
log goes to user_info/fcop.log). k32_file.c opens files O_BINARY; k32_proc.c has setenv shims.
clang's musttail needs the caller's exact signature, so generated jumps through dispatch store the
target in `c->jmp` and tail-call `dispatch_jmp(c)`. sdl2-config adds `-Dmain=SDL_main` on Windows:
build.sh strips it (our main is plain, no SDL2main). Pacing spins the last 2.5 ms before a slot on
Windows (sleeps overshoot). Test headless under Wine with its own prefix and no display, so nothing
appears on screen:
`WINEPREFIX=$PWD/build/wine WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy FCOP_SKIP_VIDEOS=1 wine fcop.exe "Z:$PWD/game_files/FCopLAPD"`
(2026-11-07: menus, mission at 1280x960, input, 60 fps paced, 75 fps unlimited). Not yet tried on
real Windows. Don't dump frames (FCOP_SHOT) to measure fps under Wine: its file writes cut it to 21.
Memory: difftest.py starts a compiler for every source at once and a worker per core; run it
with `-j 6` and nothing else heavy (the defaults plus two builds ran the machine out of memory).

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
  random FPU values; not yet checked whether it matters), 0x402800 and 0x402830 (a data
  table of `04 04 ...` that functions.json calls code; they run into each other).
- Environment switches for the game: `FCOP_TRACE_API=1` (every import call with its
  caller), `FCOP_TRACE_MSG=1` (window messages), `FCOP_DD_LOG=1` (DirectDraw blits),
  `FCOP_SHOT=build/shots/f%04d.ppm` (dump every presented frame; convert to PNG to look;
  `FCOP_SHOT_LOG=1` logs frame numbers so they line up with other log lines),
  `FCOP_TRACE_KEYS=1` (each key code GetAsyncKeyState is asked about, and every press seen),
  `FCOP_INPUT="9000:click@484,303 28000:Keypad_8*3000"` (scripted input, ms after start: a
  click at game coordinates, preceded by a mouse move, or an SDL key name held 100 ms or *N ms).
  That example starts the first mission from the main menu and walks forward.
  `t:scan=N` in the script logs where guest memory holds the 16-bit value N, later scans narrow
  that list, `t:scan=?` shows what the places hold now (found the gun's ammo counter). Then a
  `DEBUG=1 OUT=build/fcop_dbg` build in gdb: break on plat_scan16, `watch -l *(u16*)(MEM+addr)`.
  `FCOP_SHOT_AT=ms` dumps only frames from then on (dumping every frame slows the game to ~28 fps).
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
- Port settings in Preferences (dialog 101): dialog.c hides Processor Select and puts our own
  controls in the gap, "Frame Rate (missions)" (37.5 original, up to 60/90/120/144/165/240,
  Unlimited) and "Show FPS Counter". Their ids start at PORT_ID 0x7f00; every message to the
  game's dialog procedure goes through `dlg_call`, which keeps them away from it. OK saves them
  (registry Settings `FrameRate`: 37 = original, 0 = unlimited, else the cap; `ShowFPS`) and
  applies them at once (u32_win.c `win_set_fps`, which also switches vsync). Preferences only
  opens from the main menu (the pause bar has no button for it), so the rate never changes
  mid-mission. FCOP_FPS_MAX / FCOP_FPS override the saved rate for tests. The counter is drawn
  top-right into the shown picture only (FCOP_SHOT frames don't have it).
- CRT filter (branch crt-filter, experiment, aesthetics only): Preferences checkbox "CRT Filter" under
  Show FPS Counter (registry Settings `CRTFilter`, `FCOP_CRT=0/1` overrides). u32_win.c `crt_draw`, after
  the picture is drawn: a MOD-blended overlay at the picture's size in screen pixels (scanlines, an
  R/G/B aperture grille every 3 pixels, darker corners; rebuilt only when that size changes), then the
  picture again ADD-blended at alpha 64 to win back brightness. Scanlines follow picture lines while a
  line gets >= 3 screen pixels, else every 2nd/4th line. FCOP_SHOT frames never have it.
  `FCOP_SCREEN_SHOT=file.ppm` saves what the screen shows (filter, FPS counter) at every present;
  headless it needs `SDL_RENDER_DRIVER=software` (the offscreen OpenGL renderer reads back a wrong picture).
- Settings live in our registry (`user_info/registry.txt`) under
  HKLM\Software\Electronic Arts\Future Cop\Settings (Resolution = list index, FullScreen,
  volumes...). The installer used to create the key; gdi_misc.c creates it now. The game saves
  them **when it exits normally** (plat_test's kill skips that: quit with
  `click@156,306` + `Return` in the input script). `FCOP_TRACE_REG=1` logs registry calls.
- Key bindings: `user_info/controls.txt` (host_path redirects the game's `controls.txt` and
  `save.cfg` there), written **as soon as Controller
  Configuration closes with OK** (tests that rebind keys change the player's real bindings:
  back the file up and restore it). In memory: 14 (device, key) pairs per player at
  0x511c20 / 0x511ca0, actions 0..3 forward, back, right, left; the arrow keys press
  whatever player 1 has bound to those four (u32_misc.c `arrow_alias_down`).
- Display: menus are always 640x480; the chosen resolution and fullscreen apply in missions.
  Windowed, DirectDraw's primary surface is the whole desktop (we report DESKTOP_W x DESKTOP_H,
  1600x1200, win.h) and only the main window's rectangle is shown. Fullscreen comes from
  SetCooperativeLevel (SDL fullscreen-desktop, letterboxed); Alt+Enter toggles it.
- 1600x1200 is removed (user's choice, 2026-10-20): the game's own per-resolution switches
  (0x40cd3f, 0x41031c, 0x410542, 0x411181, 0x4112f1) stop at index 9, so at index 10 the 3D view
  and HUD stay in its 2560x1920 virtual space (1.6x too big, top-left). ddraw.c doesn't enumerate
  it, dialog.c drops it from the Display Mode list, gdi_misc.c turns a saved Resolution 10 into 9.
  Restoring it would mean adding the missing cases (coordinates x5/8, sizes x2.5 and x5).
- Presenting: the menu redraws in ~50 blits straight onto the primary surface. Showing each one
  (each present waits for vsync) flickered for most of a second, so ddraw.c only marks the primary
  changed and presents it on the game's next PeekMessage/GetMessage, on Flip, or after 50 ms
  (`dd_present_pending`).
- Registry `Install Dir` (HKLM\...\Future Cop) is the folder the game puts in front of file
  names; gdi_misc.c sets it to `C:\FCopLAPD` when missing. Without it the game used `.\`, and
  its BMP loader (0x413f20) takes the extension after the *first* '.' (strchr), so the legal
  screen and the mission loading pictures (Missions\Ld*.bmp) never loaded.
- ddraw.c Blt colour fills read DDBLTFX.dwFillColor at 0x50 (Gemini had 0x48: a pink flash
  before missions).
- Pixel format: ddraw.c gives the game **555** by default. In 565 the software renderer's
  "Texture Coloring" (coloured lighting) comes out wrong (yellow/red floors with green
  specks; compare `FCOP_PIXFMT=565` with ColoringIsOff 0 and 1). Root cause not found yet:
  most 1998 cards were 565 and it worked there, so suspect the recompiled 565 code path
  (FUN_0045b1c0's 555/565 patch table at 0x48ea20, or other self-modifying code).
- Videos (intro, HELP = cwrules.mpg, BRIEFING = brief*.mpg, fly-overs, credits) are MPEG-1
  files in the CD's `Video` folder (host_path falls back to the parent of the game folder).
  video.c implements the DirectShow filter graph on pl_mpeg; `FCOP_SKIP_VIDEOS=1` skips
  them (plat_test.py does unless `--videos`, so the menu still appears after about 8 s).

## Frame rate (branch fps60, experiment)

The game's clock ticks at 300 Hz (0x4130c0: QueryPerformanceCounter, us x 3 x 0.0001). The mission
loop (0x406a7f) waits until 8 ticks have passed since the last frame (37.5 fps), then passes the real
elapsed ticks (0x511f58, clamped to 60) to the game update (0x43b310): the game logic is time-based.
x86c FRAME_WAIT makes that wait ask `rt_frame_hook` (u32_win.c `win_frame_pace`): frames start every
n screen refreshes, the most frames per second up to FCOP_FPS_MAX, timed
on the host clock **at the present** (`present_paced` in win_show): the frame starts as soon as a tick
has passed and its finished picture waits for its slot. Timing the frame start instead let drawing
time (10-16 ms, varying as the camera turns) shift presents by +-3 ms, so frames missed refreshes
(choppy when the camera swings round). On the user's screen the game also read its clock a varying
time after the present (frames advanced 4..7+ ticks; headless runs never showed it), and ~70 places
read that clock (0x511ee4) directly for animations, so x86c FPS_PATCH 0x406a56 asks `rt_clock_hook`
(`win_frame_clock`): while pacing, a frame's clock is the time it will be shown (5 and 6 ticks in turn
at 55 fps; a late frame's lost time still counts). The Preferences "Frame Rate" (or `FCOP_FPS_MAX`,
default 90) caps the rate: 165 Hz
gives 82.5 fps (drawing must fit in 12.1 ms; the user found it the smoothest, steady 83 fps on the
laptop), 144 Hz 72, 120/60 Hz 60. `FCOP_FPS_MAX=0` is unlimited: no pacing, no vsync, the real clock
(at most 300 fps, the clock's resolution; ~95 fps headless at 1280x960). `FCOP_VSYNC=0` presents without SDL's vsync wait;
the FPS log shows frame spacing, the longest drawing time, present blocking and ticks per frame.
build/fps_test.sh (not committed) runs variants on the user's screen, logs in build/fps_logs/. Plain 60 fps on the user's 165 Hz laptop screen looked choppy (frames shown for 3,
3, 3, 2 refreshes) although every frame advanced exactly 5 ticks. `FCOP_REFRESH=Hz` stands in for the
screen (headless runs have none); `FCOP_FPS=N` uses the plain whole-tick wait (`rt_frame_ticks`)
instead (FCOP_FPS=37 is the original). Measured 60 fps at 1280x960, also 2-player; some places cost
13-16 ms a frame there. Not done: locking to the real vblank (SDL reports whole Hz, 165 vs 164.97, so
the phase slips by one refresh about every 30 s). Other 8-tick waits (0x4071aa, 0x407278, 0x407295,
probably the network game) are unchanged. `FCOP_FPS_LOG=1` logs frames per second, the shortest and
longest time between frames and how many ticks each advanced.

Checked at 37 vs 60 fps with the same input script (2026-11-07): walking, turning and the camera
match. The machine gun did not: its weapon delay is 0, so it fired once per frame (180 rounds in 3 s
at 60 fps instead of 113). x86c FPS_PATCH (only when rt_frame_hook is set or rt_frame_ticks < 8) makes a delay of 0 count as
8 ticks in the fire function (0x458ed0) and keeps the sub-frame remainder in the cooldown countdown
(0x45a2a0) instead of clamping it to 0; now 112 rounds at both rates, rockets/grenades unchanged.
Hover mode (Ctrl+Shift with the user's bindings) turned 14% slower at 82.5 fps and about half
speed unlimited (user noticed). Two FPS_PATCH fixes: the hover physics (0x442510) runs fixed 8-tick
substeps but built up the turn speed (+0xc2 of the player data, clamped +-96) by the frame's ticks in
each (0x442698: now 8); FUN_004456a0 eases heading/aim toward the target in 2-tick steps
(0x445eb0) but capped the move at ticks*4 / ticks*2 a frame (0x445bcd, 0x445b38: now steps*8 / *4).
Measured with `peek=4a1b9c` (player heading) every 100 ms: identical at 37.5, 82.5, unlimited;
walking unchanged (its run-to-run spread is +-32 from input timing).
Still frame-based but probably only cosmetic: 0x418af9 calls 0x418b50 (spawns something, smoke or
similar) every 4th frame (frame counter 0x511f40), so 1.6x as often at 60 fps.

## Performance (measured 2026-10-19, mission at 1280x960, game capped at 37 fps)

The main thread went from 100% of a core to about 37%:
- The game waits for frames by polling QueryPerformanceCounter / timeGetTime in a tight loop:
  `timer_now_us()` (k32_sync.c) sleeps 0.3 ms once a thread has read the clock 64 times in a
  row with < 200 us between reads. Menu idle: ~5%.
- 16-bit frames are converted to ARGB8888 through a 64K-entry table in `win_show` (SDL's own
  555 -> 8888 conversion, BlitNtoN, was 22% of the CPU).
- x86c merges functions.json fragments that code falls through into (the renderer's pixel
  loops were split mid-loop, storing all registers at every hop); fall-through tail calls
  went from 107 to 19.
- What remains is the game's own per-pixel texture loop (0x45bbbe-0x45bbf0 in F_45b6db, ~30%).
  -O1, -O2 and -O3 measure the same; release builds use -O2. `perf` is blocked on this machine
  (perf_event_paranoid 4): recomp/prof/sampler.c (LD_PRELOAD) and recomp/prof/report.py
  (usage in its docstring) profile per function and per generated line.

Profile 2026-11-07 (unlimited fps, 1280x960, ~95 fps headless): F_45b6db 36%, F_45d7e7 12%,
F_45e86d 8%, F_45be7a 4% (all renderer), GPU driver texture upload 9%, win_show 3%. Done then:
- x86c `fpu_copies`: runs of fild qword / fxch / fistp qword (the game's FPU memcpy and its frame
  copy at 0x415299) become 64-bit copies through C locals, in the same order (F_415130 5.6% -> 1.3%).
- cpu.h `rt_fist` is inline: SSE2 cvtsd2si / cvttsd2si for round-to-nearest and truncate (same
  results, INT64_MIN included); other rounding modes call rt_fist_slow (was 2% as a call).
- win_show converts 555/565 with shifts (the compiler vectorises them) instead of the 256 KB table:
  0.37 vs 0.59 ms a frame in isolation, not measurable in the game. ENV_ON (host.h) caches getenv
  for switches asked on every Blt / key poll.
Tried and dropped: keeping the texture loop's patched immediates (rd32(0x45bbc3) etc.) in C locals
instead of reading guest memory per pixel. Slower (95/90 vs 100-104 fps): the loop already uses
every register, and the reads hit L1. Headless fps varies +-5 with machine load: compare builds
by running them alternately.

## Next steps

1. Done: menus, missions in both modes (incl. 2-player split screen), sound and music,
   dialogs, settings, resolutions/fullscreen, arrow keys, key rebinding, gamepads (via SDL,
   tested with the virtual pad only). Next: play longer (FCOP_INPUT scripts), check other missions and the
   in-game Esc menu, map arrow keys/WASD as extra defaults later (preferences menu).
2. Fix in-game crashes and rendering (expect more stub/offset bugs in Gemini's code;
   review ddraw.c, dsound.c, dinput.c against the DirectX 5 SDK layouts).
3. Sound through dsound.c and SDL audio; music streaming uses the play/write cursors.
4. Hardware rendering was explored and dropped (see Goal 3). Facts kept for reference: the
   game picks its renderer from registry `Group` under HKLM\Software\Electronic Arts\Future
   Cop: "Software" (default), "3Dfx" (loads glide2x.dll, Glide 2), anything else loads the
   THRASH driver named by "Thrash Driver" + "a.dll" (DLLs in game_files/setup/thrash).
5. Open: the pause bar shows black behind it (the game clears the screen), the 565
   Texture Coloring root cause, gamepads on real hardware, the game's own menu font instead
   of font8x8.
