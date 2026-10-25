# Glide Renderer for Missions (Glide 2.x on OpenGL) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game's built-in 3dfx Glide mission renderer runs on OpenGL in our code, at the real screen resolution, selectable permanently with `Group` = `3Dfx` in registry.txt; OpenGL (THRASH) stays the default.

**Architecture:** The game loads `glide2x.dll` with `LoadLibraryA` and fetches 121 functions with `GetProcAddress` (loader 0x411680). k32_proc.c answers with a pseudo module whose exports are thunks into a new `glide_gl.c`, exactly as `thrash_gl.c` does for THRASH. `glide_gl.c` keeps Glide's state (combine modes, texture memory, clip window) and draws through the shared `gl_present.c` context, which gains the few features Glide needs (combine modes, scissor, gamma).

**Tech Stack:** C (MinGW gcc on Windows, gcc on Linux), SDL2, OpenGL 3.3 core via `SDL_GL_GetProcAddress`; Python 3 + capstone for reading the game's code.

**Spec:** `docs/superpowers/specs/2026-10-25-glide-renderer-design.md` (its "How the game uses Glide" section is the fact base).

## Global Constraints

- Never commit anything derived from the EXE (generated C, extracted data, copied tables). Notes describing behaviour and addresses are fine. No code copied from the Glide SDK (3dfx licence); our own `glide.h` with the constants we need.
- OpenGL (THRASH) stays the default: a fresh registry.txt gets `Group` = `OpenGL`. `3Dfx` selects Glide, `Software` the software renderer. `FCOP_RENDERER=glide|opengl|software` overrides.
- Render at the real drawable size: game vertices are transformed, never a low-resolution image scaled up; 4:3 pillarbox.
- Resolution list in Glide mode = the full list of OpenGL mode (320x240..1280x960, indices 0-9).
- Gamma applies to the whole Glide mission picture (HUD included), at present time.
- THRASH output and the software renderer must not change.
- Linux keeps compiling: `#ifdef _WIN32` for Windows-only code; no `__attribute__((weak))` on data.
- Commit locally on `windows-build`; push only when the user asks, and only to remote `fork`.
- Build/test: `.venv/Scripts/python.exe -I recomp/plat_test.py ...` from PowerShell with `C:\msys64\mingw64\bin;C:\msys64\usr\bin` on PATH. Runs that change `game_files/FCopLAPD/registry.txt` or `controls.txt` back them up first and restore them after.

## Review Focus

1. **Mission restart / second mission / back to menu and in again**: the loader runs again (`GetProcAddress` x121, `grGlideInit`, `grSstWinOpen`); no second thunk set, no stale texture memory, no crash. → Task 5 test.
2. **Esc pause bar and dialogs over a Glide frame**: `glp_read_frame16` must return the Glide picture (with gamma, as the player saw it), not black or a menu frame. → Task 5 test.
3. **Resolution change and Alt+Enter mid-mission** (`grSstControl`, window close/reopen): picture stays correct and sharp, mouse mapping still right. → Task 5 test.
4. **OpenGL cannot start with `Group` = `3Dfx`** (`FCOP_GL_FAIL=1`): game falls back to software and stays playable. → Task 5 test (Step 3).
5. **Settings saved on exit** keep `Group` = `3Dfx` (the game rewrites values on normal exit). → Task 2 test.

---

### Task 1: Glide facts (reverse engineering notes)

No product code. Produces the values every later task pins.

**Files:**
- Create: `docs/GLIDE_NOTES.md`
- Spike (not merged): branch `spike/glide-probe`, file `recomp/platform/glide_probe.c`

**Interfaces:**
- Produces (in GLIDE_NOTES.md): the loader's handling of a NULL `GetProcAddress` result; the start-up/mission-start call sequence; each `grSstQueryHardware` field the game reads (0x43a300, 0x43a380) and the answer that offers resolution indices 0-9; the `GR_RESOLUTION_*` code per index (switch at 0x43a490) and our size for each code; the `GrVertex` fields the game fills (expected Glide 2.x layout: x, y, z, r, g, b, ooz, a, oow, then tmuvtx[0] sow, tow, oow at +0x24; 72 bytes) and their value ranges; the exact list of (`grColorCombine`, `grAlphaCombine`, `grTexCombine`, `grAlphaBlendFunction`, `grConstantColorValue`) combinations 0x4393e0 and 0x43a540 set, with what each primitive type uses; chromakey mode/value and how often they change; `grTexSource`/`grTexDownloadMipMap*` arguments (addresses, `GrTexInfo` fields: expected smallLod 0, largeLod 0 = 256, aspect 3 = 1:1, format 0xb = ARGB1555) and the partial-download pattern; `grClipWindow` use in split screen; `grBufferClear` arguments; `grGammaCorrectionValue` input (0x511fc8 * k - c, values seen); `grSstControl` arguments and when; what the ~25 readers of 0x511fa4 change (texture preparation first); what the game does when `LoadLibraryA("glide2x.dll")` returns 0.

- [ ] **Step 1: Read the game's Glide code** (0x411680 loader, 0x4393e0 backend, 0x43a240-0x43a680 helpers, 0x415be0 start, 0x45a340, 0x45aa90/0x45ac80 selection) in `build/recomp/gen/*.c` (`grep "TR(0x4393e0u)"`) and with capstone; look up API constants in the Glide 2.x SDK headers (https://github.com/Danaozhong/3dfx-Glide-API).
- [ ] **Step 2: Probe** on `git checkout -b spike/glide-probe`: `glide_probe.c` resolves every `_gr*`/`_gu*` name to a thunk that logs its name and arguments (vertices: dump the first of each frame) and returns 1 / plausible values (`grSstQueryBoards` 1 board, `grSstQueryHardware` Voodoo 2 with 4 MB texture memory, `grTexMinAddress` 0, `grTexMaxAddress` 4 MB); hook it in k32_proc.c. Run with `Group`=`3Dfx` (back up registry.txt): mission 1 one player, 2-player split screen, a Precinct Assault match, a resolution change and Esc pause. Record calls not in the spec's list.
- [ ] **Step 3: Write `docs/GLIDE_NOTES.md`** in the style of docs/THRASH_NOTES.md (behaviour and addresses only), with one worked 1555 pixel (`0x7c1f` -> R ff, G 00, B ff, A 00; `0xfc00` -> ff 00 00 ff).
- [ ] **Step 4: Commit on `windows-build`**: `git checkout windows-build && git add docs/GLIDE_NOTES.md && git commit -m "docs: Glide interface notes"`. The spike branch stays local.

### Task 2: Renderer selection (Glide as third choice) and fallback

**Files:**
- Create: `recomp/platform/renderer.h`
- Modify: `recomp/platform/gdi_misc.c:531-545` (`thrash_wanted`), `:671-676` (forced `Group`)
- Modify: `recomp/platform/u32_win.c:273` (GL context also for Glide)
- Modify: `recomp/platform/thrash.h` (comment of `thrash_wanted`)

**Interfaces:**
- Produces (`renderer.h`): `enum { REND_SOFTWARE, REND_OPENGL, REND_GLIDE }; int renderer_wanted(void);` env `FCOP_RENDERER` (`software`/`opengl`/`glide`, case-insensitive) wins, else `Group`: `Software` -> REND_SOFTWARE, `3Dfx` -> REND_GLIDE, anything else or missing -> REND_OPENGL.
- Produces: `thrash_wanted()` == `renderer_wanted() == REND_OPENGL`. `RegQueryValueExA` of `Group` with `FCOP_RENDERER` set answers `Software` / `OpenGL` / `3Dfx`.
- Produces: u32_win.c creates the GL window/context when `renderer_wanted() != REND_SOFTWARE`.

- [ ] **Step 1: Test (fails now)**: `$env:FCOP_RENDERER="glide"; .venv/Scripts/python.exe -I recomp/plat_test.py --seconds 30 --input "9000:click@484,303"`; expected after the change: log line `LoadLibraryA('glide2x.dll') -> NULL` (no Glide module yet) and the mission runs (software fallback until Task 4). Before: THRASH loads instead.
- [ ] **Step 2: Implement** `renderer_wanted()` in gdi_misc.c, `thrash_wanted` on top of it, the three forced strings, and the u32_win.c condition.
- [ ] **Step 3: Verify**: Step 1 run passes; without `FCOP_RENDERER` and a registry.txt without `Group`, the log shows `THRASH: opengla.dll -> OpenGL` and registry.txt gets `Group`=`OpenGL`; with `Group`=`3Dfx` written in registry.txt (no env), the `glide2x.dll` line appears.
- [ ] **Step 4: Verify save on exit**: `Group`=`3Dfx` in registry.txt, input `"9000:click@156,306 10500:Return"`; afterwards registry.txt still has `Group`=`3Dfx`.
- [ ] **Step 5: Commit**: `git commit -am "Renderer choice: Group=3Dfx selects Glide, FCOP_RENDERER=glide"`

### Task 3: Shared texture conversion and Glide texture memory (no GL, unit-tested)

**Files:**
- Create: `recomp/platform/texconv.c`, `recomp/platform/texconv.h` (replace `thrash_tex.c/.h`; `git mv`)
- Create: `recomp/platform/glide_tmem.c`, `recomp/platform/glide_tmem.h`
- Modify: `recomp/platform/thrash_gl.c` (new names), `recomp/build.sh` if it lists files
- Test: `recomp/test_texconv.c` (from `test_thrash_tex.c`), `recomp/test_glide_tmem.c`

**Interfaces:**
- Produces (`texconv.h`): `void tex_4444_to_rgba8(const uint16_t *src, uint8_t *dst, int n);` (old `thrash_tex_to_rgba8` behaviour), `void tex_1555_to_rgba8(const uint16_t *src, uint8_t *dst, int n, int key_on, uint16_t key);` (5-bit x -> `x << 3 | x >> 2`; A = bit 15 ? 0xff : 0; with `key_on`, texels whose low 15 bits equal `key & 0x7fff` get A = 0), `uint32_t tex_hash(const uint16_t *src, int n);` (FNV-1a over 2n bytes, same values as `thrash_tex_hash`).
- Produces (`glide_tmem.h`): texture memory of `GTM_SIZE` = 4 MB (or the Task 1 value): `void gtm_reset(void);`, `int gtm_download(uint32_t addr, int lod_w, int lod_h, const uint16_t *px);` (copies `lod_w*lod_h` 16-bit texels, marks every entry overlapping `[addr, addr + 2*w*h)` dirty; 0 when out of range), `int gtm_download_rows(uint32_t addr, int w, int h, int row0, int row1, const uint16_t *px);` (partial level: rows row0..row1 inclusive), `const uint16_t *gtm_texels(uint32_t addr, int w, int h, int *dirty);` (pointer into memory; `*dirty` = 1 once after any write overlapping it, then 0), plus the `GR_LOD_*` -> size helper `int gtm_lod_size(int lod);` (0 -> 256 ... 8 -> 1).

- [ ] **Step 1: Write the failing tests**: test_texconv.c keeps every 4444 and hash case of test_thrash_tex.c and adds 1555: `0x7c1f` -> ff 00 ff 00, `0xfc00` -> ff 00 00 ff, `0x8421` -> 08 08 08 ff, key on with key `0x0000`: `0x8000` -> A 00, `0x8001` -> A ff. test_glide_tmem.c: download 256x256 at 0, `gtm_texels(0,256,256)` dirty 1 then 0; download at 0x20000 leaves address 0 clean; partial rows 10..11 at 0 dirties 0 and the texel row 10 matches; download at `GTM_SIZE - 2` of a 2x1 texture returns 0; `gtm_lod_size(0)` 256, `(8)` 1.
- [ ] **Step 2: Run, expect failure**: build lines at the top of each test file (same style as test_thrash_tex.c), e.g. `gcc -Irecomp/rt -Irecomp/platform recomp/test_glide_tmem.c recomp/platform/glide_tmem.c -o build/test_glide_tmem && build/test_glide_tmem` -> compile error (missing file).
- [ ] **Step 3: Implement** texconv.c and glide_tmem.c (entries: a small table of {addr, w, h, dirty} created on first `gtm_texels`; downloads mark overlapping entries dirty). Switch thrash_gl.c to `tex_4444_to_rgba8` / `tex_hash`.
- [ ] **Step 4: Run, expect `ok`** from both tests; `plat_test.py --seconds 28 --input "9000:click@484,303" --png build/shots/thrash.png` (THRASH mode) still shows mission 1 textured.
- [ ] **Step 5: Commit**: `git add -A recomp && git commit -m "Shared texture conversion (4444, 1555, hash) and Glide texture memory"`

### Task 4: glide_gl.c: Glide module, set-up, drawing

**Files:**
- Create: `recomp/platform/glide_gl.c`, `recomp/platform/glide.h`
- Modify: `recomp/platform/k32_proc.c:86-107` (try `glide_load` / `glide_proc` after THRASH)
- Modify: `recomp/platform/gl_present.c`, `gl_present.h` (Glide features)
- Test: `recomp/test_gl_present.c` (combine-mode table, if pure helpers are added)

**Interfaces:**
- Consumes: `renderer_wanted()` (Task 2), `tex_1555_to_rgba8`, `tex_hash`, `gtm_*` (Task 3), Task 1's combination list and resolution table.
- Produces (`glide.h`): `u32 glide_load(const char *dll_name);` (case-insensitive `glide2x.dll`, Glide wanted and `glp_active()` -> pseudo module `0x61000000`, else 0), `u32 glide_proc(u32 module, const char *name);` (every one of the 121 names gets a thunk, made once; unimplemented ones log once and return 0/1 as Task 1 says is safe), `extern int glide_frame_last;` (1 while the picture on screen is a Glide frame; u32_win.c treats it like `thrash_frame_last`).
- Produces (`gl_present.h`): `void glp_set_combine(int mode);` with one `GLP_CC_*` constant per combination in Task 1's list (default `GLP_CC_TEX_X_VERT` = today's shader), `void glp_set_const_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);`, `void glp_set_alpha_test(int on);` (discard A < 0.5: chromakeyed texels), `void glp_set_clip(int x0, int y0, int x1, int y1);` (game coordinates, scissor; full frame after `glp_begin_frame`), `void glp_set_gamma(float g);` (1.0 = off; applied in the pass that shows the offscreen framebuffer, in `FCOP_SHOT` shots and in `glp_read_frame16`; reset to 1.0 by THRASH and 16-bit frames).
- Vertex mapping: x, y as is; z = ooz / 65535 (no depth test: the game sorts); q = oow; colour = clamp(r, g, b, a) to 0..255; u, v = (sow / oow, tow / oow) / 256 for 256-texel textures (texel units divided by the texture size from `gtm_lod_size`).
- Coordinate space: the size of the `GR_RESOLUTION_*` code from the last `grSstWinOpen` (Task 1 table).
- Textures: `grTexSource(tmu, addr, evenOdd, info)` -> `gtm_texels`; when dirty or the chromakey changed, convert with `tex_1555_to_rgba8` and upload to the GL texture kept per address; filter from `grTexFilterMode` unless `FCOP_TEXFILTER`; clamp from `grTexClampMode`.

- [ ] **Step 1: Test (fails now)**: `$env:FCOP_RENDERER="glide"; plat_test.py --seconds 30 --input "9000:click@484,303" --png build/shots/glide_m1.png`; expected: log `Glide: glide2x.dll -> OpenGL`, no `-> NULL` GetProcAddress lines, the PNG shows mission 1 (scene, HUD, radar, text) like `build/shots/thrash.png` from Task 3. Today: software fallback.
- [ ] **Step 2: Implement set-up** (`grGlideInit`, `grSstQueryBoards`, `grSstQueryHardware`, `grSstSelect`, `grSstWinOpen` -> `glp_begin_frame` size, `grSstWinClose`, `grSstControl`, `grSstIdle`, `grGlideShutdown`, `grTexMinAddress`/`grTexMaxAddress` -> 0 / `GTM_SIZE`), frame (`grBufferClear` -> begin frame with that colour, `grBufferSwap` -> `glp_swap`, `glide_frame_last = 1`, FPS log like THRASH with `FCOP_FPS`), drawing (`grDrawTriangle`, `guDrawTriangleWithClip`, `grDrawLine`, `grDrawPoint` as a 1-pixel quad or GL point), textures, combine/blend/chromakey/gamma/clip states, and the hook in k32_proc.c.
- [ ] **Step 3: Run Step 1** and compare the PNG with thrash.png side by side; also `FCOP_TEXFILTER=nearest` once.
- [ ] **Step 4: Unit tests still pass**: test_gl_present, test_texconv, test_glide_tmem.
- [ ] **Step 5: Commit**: `git add recomp && git commit -m "Glide 2.x renderer on OpenGL (glide_gl.c)"`

### Task 5: All modes, robustness, docs

**Files:**
- Modify: `recomp/platform/glide_gl.c`, `gl_present.c`, `u32_win.c` as the tests require
- Modify: `CLAUDE.md` (renderer bullet: `Group` = `3Dfx`, `FCOP_RENDERER=glide`, glide_gl.c in the layout list), `docs/GLIDE_NOTES.md` (anything learned)

- [ ] **Step 1: 2-player split screen and Precinct Assault** in Glide mode (`--input` scripts as used for THRASH; see docs/PLAYTEST_TASK.md): PNGs match the OpenGL-mode PNGs of the same moment in layout; both views drawn, HUD in place.
- [ ] **Step 2: Review Focus 1-3**: mission start, Esc + back to menu, start again (log: one `Glide: glide2x.dll -> OpenGL` per start, no new thunk count, no errors); Esc pause during a mission (`--png` shows the bar over the Glide picture); a resolution index change 640x480 -> 1280x960 in Preferences and Alt+Enter (`--show` run by hand): sharp picture, correct HUD, clicks land.
- [ ] **Step 3: Fallback**: `Group`=`3Dfx` + `FCOP_GL_FAIL=1`: log shows Glide not available, mission runs in software.
- [ ] **Step 4: FPS**: `FCOP_FPS=1` mission 1, Glide vs OpenGL mode: Glide not lower.
- [ ] **Step 5: Linux compile check by reading** (no `_WIN32`-only calls outside guards) and the unit tests.
- [ ] **Step 6: Commit**: `git commit -am "Glide renderer: split screen, restarts, docs"`

### Task 6: Differences review with the user

- [ ] **Step 1:** Take matching `--png` shots in Glide and OpenGL mode (mission 1 start, an explosion/transparency, the HUD, 2-player), list every visible difference in a short table (what, where, which Glide state causes it).
- [ ] **Step 2:** Show the shots and the table to the user; fix what they call a bug; record what they call the 3dfx look in GLIDE_NOTES.md. Commit.
