# OpenGL Renderer for Missions (THRASH on OpenGL) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Missions render through OpenGL on the GPU at the real screen resolution, with texture filtering, by implementing the game's THRASH driver interface natively; the software renderer stays as fallback.

**Architecture:** The game loads a THRASH driver with `LoadLibraryA` + `GetProcAddress`; our k32_proc.c answers with a pseudo module whose 34 `_THRASH_*` exports are thunks into `thrash_gl.c`. That file turns the game's pre-projected triangles into batched OpenGL 3.3 draws. In OpenGL mode every frame (menus, dialogs, videos, missions) is shown through one GL context owned by `gl_present.c`; the SDL_Renderer path stays for software mode.

**Tech Stack:** C (gcc, MinGW on Windows, gcc on Linux), SDL2 (window + GL context + `SDL_GL_GetProcAddress`), OpenGL 3.3 core, no new libraries. Python 3 + capstone/pefile for reverse engineering.

**Spec:** `docs/superpowers/specs/2026-10-25-opengl-thrash-renderer-design.md` (read it first; its "How the game uses THRASH" section is the fact base).

## Global Constraints

- Never commit anything derived from the EXE or the CD's DLLs (generated C, extracted data, copied code or tables). Notes describing behaviour and addresses are fine.
- OpenGL is the default; software stays selectable and is used automatically when OpenGL cannot start.
- Work locally only: commit on branch `windows-build`; never push, merge into `main`, or open PRs.
- The port never reads `opengla.dll` (or any THRASH DLL) at run time.
- OpenGL 3.3 core through SDL2, functions loaded with `SDL_GL_GetProcAddress`; code kept GLES 3 friendly (no fixed function, no GL_QUADS).
- 4:3 picture, centred, full window height (pillarbox) until the widescreen project.
- Bilinear texture filtering by default; nearest selectable.
- Do not change the game's own frame timing.
- Linux must keep compiling: guard Windows-only code with `#ifdef _WIN32`; no `__attribute__((weak))` on data (see CLAUDE.md).
- Build/test from the MSYS2 MinGW shell, or from PowerShell with `C:\msys64\mingw64\bin;C:\msys64\usr\bin` on PATH. Test runs that change `game_files/FCopLAPD/registry.txt` or `controls.txt` back them up first and restore them after.
- Codex (`codex exec`) may take a well-specified sub-task or do a review, but only after asking the user each time it would write files.

## Review Focus

1. **Esc pause bar and dialogs during a mission** (they draw over `win_last_frame()`): in OpenGL mode the last frame must be the GL picture (read back to 16-bit), not stale or black. → Task 6 test.
2. **Alt+Enter / fullscreen toggle and window resize mid-mission**: the GL context, textures and viewport must survive (or be recreated) without a crash or black picture. → Task 6 test.
3. **Second mission start / mission restart / back to menu and in again**: `LoadLibraryA`, `init`, `setvideomode`, `talloc` run again; no leaked or stale textures, no second context. → Task 6 test.
4. **OpenGL unavailable** (context creation fails): the game must fall back to software without the player doing anything. → Task 2 test.
5. **Settings saved on exit**: the game's normal exit save must not lose or break the renderer choice. → Task 2 test.

---

### Task 1: THRASH facts (reverse engineering notes)

No code. Produces the facts every later task pins its values on.

**Files:**
- Create: `docs/THRASH_NOTES.md`

**Interfaces:**
- Produces: the meanings later tasks implement: each `setstate` id/value seen (spec list), the `_THRASH_about` fields the game reads, vertex dword +0x14, `drawtrimesh(n, verts, ?)` argument layout, texture format 7 bit layout, `setvideomode(a, b, c)`, `window(n)`, `restore(...)`, `treset()`, `talloc` arguments 3-5 and its return value, `init`/`is` return-value meaning, `pageflip`/`flushwindow` order, and the coordinate space of vertex x/y (which width/height).

- [ ] **Step 1: Read opengla.dll's implementations** of `setstate`, `drawtri`, `drawtrimesh`, `drawline`, `talloc`, `tupdate`, `settexture`, `setvideomode`, `window`, `restore`, `treset` with capstone (`game_files/setup/thrash/opengla.dll`, base 0x60000000; it resolves opengl32 entry points into a table at 0x6000e1xx: map those slots to GL function names from its `GetProcAddress` strings). Cross-check unclear cases in `d3da.dll`.
- [ ] **Step 2: Read the game's side**: the about-struct fields it reads after `about()` (0x46b878 onwards, 0x4570e1 onwards), and its `setvideomode`/`setstate` call sites (0x456f80-0x457020, 0x456700-0x4567f0).
- [ ] **Step 3: Probe the other modes** on branch `spike/thrash-probe` (`git checkout spike/thrash-probe`; registry lines as in the spec; back up registry.txt): 2-player split screen and a Precinct Assault match, recording any THRASH function or setstate pair not in the spec's list. Return to `windows-build` afterwards.
- [ ] **Step 4: Write `docs/THRASH_NOTES.md`**: one section per function with argument meanings, return values, and the GL equivalent opengla uses; a table of setstate ids; the vertex layout; texture format 7 with one worked pixel (e.g. `0xf001` -> RGBA bytes).
- [ ] **Step 5: Commit**: `git add docs/THRASH_NOTES.md && git commit -m "THRASH interface notes"`

### Task 2: Renderer selection and fallback

**Files:**
- Modify: `recomp/platform/gdi_misc.c` (registry defaults and query override)
- Create: `recomp/platform/thrash.h`
- Modify: `CLAUDE.md` (settings documentation)

**Interfaces:**
- Produces (`thrash.h`): `int thrash_wanted(void);` 1 when OpenGL should be offered: env `FCOP_RENDERER` (`opengl`/`software`) wins, else registry value `Group` under `HKEY_LOCAL_MACHINE\Software\Electronic Arts\Future Cop` != `Software`.
- Produces: registry defaults written when absent at registry load: `Group` = `"OpenGL"` (REG_SZ), `Thrash Driver` = `"opengl"` (REG_SZ). `RegQueryValueExA` of `Group` answers `"Software"` when `FCOP_RENDERER=software`.
- Produces: env `FCOP_GL_FAIL=1` makes OpenGL start-up fail on purpose (used by Task 3's `init`).

- [ ] **Step 1: Test (manual run, fails now)**: run `.venv/Scripts/python.exe -I recomp/plat_test.py --seconds 28 --input "9000:click@484,303"` with `FCOP_TRACE_REG=1` and the registry.txt from before this task; expected after the change: `registry.txt` gains the two values and the log shows `LoadLibraryA('opengla.dll')`. Before: neither.
- [ ] **Step 2: Implement** the defaults in the registry loader of gdi_misc.c (after `reg_load`, where the Settings key is created today) and the `FCOP_RENDERER=software` override in `RegQueryValueExA`.
- [ ] **Step 3: Verify**: the run above shows the values and the `LoadLibraryA` line (it still returns NULL until Task 3, so the mission runs in software; that is the expected fallback). With `FCOP_RENDERER=software`: no `LoadLibraryA` line.
- [ ] **Step 4: Verify save on exit**: run with input `"9000:click@156,306 10500:Return"` (quit from the menu so the game saves); `registry.txt` still has `Group`=`OpenGL` afterwards.
- [ ] **Step 5: Document** in CLAUDE.md under the settings bullet: `Group` (`OpenGL`/`Software`), `FCOP_RENDERER`, `FCOP_GL_FAIL`.
- [ ] **Step 6: Commit**: `git commit -am "Offer the THRASH (OpenGL) renderer by default; FCOP_RENDERER=software"`

### Task 3: THRASH module and GL context

**Files:**
- Create: `recomp/platform/thrash_gl.c`
- Create: `recomp/platform/gl_present.c`, `recomp/platform/gl_present.h`
- Modify: `recomp/platform/thrash.h` (add `thrash_load`, `thrash_proc`)
- Create: `recomp/test_gl_present.c`
- Modify: `recomp/platform/k32_proc.c` (`imp_LoadLibraryA`, `imp_GetProcAddress`)
- Modify: `recomp/platform/u32_win.c` (create the SDL window with `SDL_WINDOW_OPENGL` when `thrash_wanted()`)

**Interfaces:**
- Consumes: `thrash_wanted()` (Task 2), `docs/THRASH_NOTES.md` (Task 1).
- Produces (`thrash.h`): `u32 thrash_load(const char *dll_name);` returns the pseudo module handle `0x60000000` for a name ending in `a.dll` (case-insensitive) when `thrash_wanted()`, else 0. `u32 thrash_proc(u32 module, const char *name);` returns a `thunk_new` thunk for `_THRASH_<fn>@N` (match the name up to `@`), else 0.
- Produces (`gl_present.h`): `int glp_init(SDL_Window *w);` (creates the context once, loads GL functions, returns 0 on failure or when `FCOP_GL_FAIL=1`), `int glp_active(void);`, `void glp_viewport_43(int game_w, int game_h, int out_xywh[4]);` (largest centred 4:3 rectangle in the drawable), `void glp_swap(void);` (swap + optional `FCOP_SHOT` readback), `int glp_read_frame16(u16 *dst, int w, int h);` (read back the last frame scaled to w x h as RGB565 for `win_last_frame`).
- Produces: `_THRASH_about` returns a guest struct allocated once with `galloc`, holding only the fields Task 1 found the game reads (+0 must not be `'XFD3'`, +8 = 0x68).

- [ ] **Step 1: Unit test for `glp_viewport_43`** in `recomp/test_gl_present.c` (built by `gcc -Irecomp/rt -Irecomp/platform recomp/test_gl_present.c -o build/test_gl_present`; it includes `gl_present.c` with `GLP_NO_GL` defined so no GL is needed): drawable 2560x1440 -> `{320, 0, 1920, 1440}`; 1600x1200 -> `{0, 0, 1600, 1200}`; 1280x1024 -> `{0, 32, 1280, 960}`.
- [ ] **Step 2: Run it, expect a link/compile failure** (function missing).
- [ ] **Step 3: Implement** `gl_present.c` (viewport function, context creation on the main window, GL loader) and `thrash_gl.c` with all 34 entry points: the 17 used ones per THRASH_NOTES.md (drawing ones as stubs that return success for now), the 17 unused ones as no-ops that log their first call. `init` calls `glp_init` and returns the failure value from the notes when it fails. `pageflip` clears to black and calls `glp_swap`.
- [ ] **Step 4: Run the unit test**: `build/test_gl_present` prints `ok`.
- [ ] **Step 5: Game test**: `plat_test.py --seconds 30 --input "9000:click@484,303" --show` reaches the mission with no crash; log shows `THRASH init ok` and about 40+ pageflips per second (log a once-per-second FPS line from `pageflip`: `THRASH fps N`). With `FCOP_GL_FAIL=1`: log shows the fallback and the mission renders in software (`--png` shows the scene).
- [ ] **Step 6: Commit**: `git commit -am "THRASH module on OpenGL: context, entry points, fallback"` (with the new files added).

### Task 4: Menus, dialogs and videos through GL

**Files:**
- Modify: `recomp/platform/u32_win.c` (`win_show`, `ensure_window_sdl`, `win_set_fullscreen`, `win_last_frame`)
- Modify: `recomp/platform/gl_present.c`
- Modify: `recomp/plat_test.py` (OpenGL mode: hidden real window instead of `SDL_VIDEODRIVER=offscreen`)

**Interfaces:**
- Consumes: `glp_*` (Task 3).
- Produces (`gl_present.h`): `void glp_show16(const void *px, int w, int h, int pitch, u32 rmask, u32 gmask, u32 bmask);` draws a 16-bit frame (555 or 565) as one textured quad in the 4:3 viewport, nearest filtering, and swaps.

- [ ] **Step 1: Test (before change)**: `plat_test.py --seconds 15 --png build/shots/menu_sw.png` with `FCOP_RENDERER=software`; keep the PNG as the reference.
- [ ] **Step 2: Implement**: when `glp_active()`, `win_show` calls `glp_show16` instead of the SDL_Renderer path; the SDL_Renderer is never created in that mode. plat_test.py: unless `FCOP_RENDERER=software`, set no `SDL_VIDEODRIVER` and pass `FCOP_HIDDEN=1`, which u32_win.c honours by never calling `SDL_ShowWindow` (unless `--show`).
- [ ] **Step 3: Verify**: `plat_test.py --seconds 15 --png build/shots/menu_gl.png` gives the same menu as `menu_sw.png` (compare by eye; pixel diff of the 640x480 content is 0 because `FCOP_SHOT` writes the game's 16-bit frame). A dialog (`--input "9000:click@156,243"`, Preferences) and the intro video (`--videos --seconds 12`) show.
- [ ] **Step 4: Commit**: `git commit -am "Show menus, dialogs and videos through the GL context in OpenGL mode"`

### Task 5: Textures

**Files:**
- Create: `recomp/platform/thrash_tex.c` (pure conversion, no GL), `recomp/test_thrash_tex.c`
- Modify: `recomp/platform/thrash_gl.c` (`talloc`, `tupdate`, `treset`, `settexture`)

**Interfaces:**
- Produces: `void thrash_tex_to_rgba8(const u16 *src, u8 *dst_rgba, int w, int h, int fmt);` (fmt 7 per THRASH_NOTES.md). Texture table: handle (as returned by `talloc`) -> GL texture name, GL filter from env `FCOP_TEXFILTER` (`linear` default, `nearest`) or registry.txt value `TextureFilter` under the Future Cop key.
- Produces: `u32 thrash_tex_hash(const u16 *src, int w, int h);` (FNV-1a over the pixels) stored per handle, unused for now (texture replacement later).

- [ ] **Step 1: Unit test** `recomp/test_thrash_tex.c`: if Task 1 confirmed ARGB4444, `0xf001` -> `{0x00,0x00,0x11,0xff}` (R,G,B,A), `0x0fff` -> `{0xff,0xff,0xff,0x00}`, `0x8421` -> `{0x44,0x22,0x11,0x88}`; otherwise the notes' worked pixel and two more from its layout. Hash of 4 zero pixels is a fixed value printed once and then pinned.
- [ ] **Step 2: Run, expect failure** (`gcc ... recomp/test_thrash_tex.c recomp/platform/thrash_tex.c -o build/test_thrash_tex`).
- [ ] **Step 3: Implement** the converter, hash, and the four THRASH functions (`treset` deletes every GL texture and empties the table).
- [ ] **Step 4: Run unit test**: prints `ok`.
- [ ] **Step 5: Commit**: `git commit -am "THRASH textures: format conversion, GL upload, filtering"`

### Task 6: Drawing, states, and all modes

**Files:**
- Modify: `recomp/platform/thrash_gl.c` (`drawtri`, `drawtrimesh`, `drawline`, `setstate`, `window`, `restore`, `flushwindow`, `pageflip`)
- Modify: `recomp/platform/u32_win.c` (`win_last_frame` in OpenGL mission mode)

**Interfaces:**
- Consumes: Tasks 3-5.
- Vertex shader input per vertex: position from x, y (game coordinates per THRASH_NOTES.md, mapped into `glp_viewport_43`), depth and 1/w for perspective-correct u,v, colour (ARGB u32 -> normalized), the +0x14 dword as the notes say. Batching: append to a vertex array; flush on texture or state change, `drawline`, `flushwindow` and `pageflip`.

- [ ] **Step 1: Reference shots in software**: with `FCOP_RENDERER=software`, `plat_test.py --seconds 30 --input "9000:click@484,303 28000:Keypad_8*3000" --png build/shots/m1_sw.png`.
- [ ] **Step 2: Implement** drawing, the setstate table from THRASH_NOTES.md, and for `win_last_frame`: after each mission `pageflip` keep a 16-bit copy via `glp_read_frame16` at the game's resolution (only when a dialog or the pause bar asks: read lazily).
- [ ] **Step 3: Verify mission 1**: same command without `FCOP_RENDERER` -> `build/shots/m1_gl.png` shows the same scene, HUD, radar and text as `m1_sw.png` (by eye), textures filtered; log `THRASH fps` about the same as the probe's ~45.
- [ ] **Step 4: Verify the review-focus cases**, each a `plat_test.py` run with `--png`: Esc pause bar in a mission (`... 28000:Escape`): bar over the mission picture, not black; Alt+Enter twice during the mission (`--show`, keys `LAlt` held with `Return`: add a combo to FCOP_INPUT if missing) then a frame: picture intact; quit the mission to the menu and start again: second mission start renders; 2-player split screen and a Precinct Assault match (inputs found in Task 1 step 3): both render.
- [ ] **Step 5: Measure**: `--show`, fullscreen at 2560x1440: `THRASH fps` with drawing on vs `FCOP_THRASH_NODRAW=1` (skip GL draws) differ by less than 5%. Record both numbers and the software renderer's FPS (log a frame counter per second in `win_present`) in the commit message.
- [ ] **Step 6: Commit**: `git commit -am "THRASH drawing on OpenGL: batching, states, all modes"`

### Task 7: Review and documentation

**Files:**
- Modify: `CLAUDE.md` (how the OpenGL renderer works, its files, switches, known limits; replace "Next steps" item 4 with the outcome)

- [ ] **Step 1: Independent review**: ask the user, then run Codex read-only: `codex exec --sandbox read-only "Review the diff windows-build~N..windows-build in this repo for correctness bugs in recomp/platform/thrash_gl.c, gl_present.c, thrash_tex.c and the u32_win.c/k32_proc.c/gdi_misc.c changes; report file:line findings"` (N = commits of this plan). Fix confirmed findings.
- [ ] **Step 2: Linux compile check by eye**: grep the new files for Windows-only headers/APIs outside `#ifdef _WIN32`.
- [ ] **Step 3: Update CLAUDE.md**, then commit: `git commit -am "CLAUDE.md: OpenGL renderer"`.
