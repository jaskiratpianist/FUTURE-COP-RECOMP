# Glide renderer for missions (Glide 2.x on OpenGL) — design

Date: 2026-10-25. Status: builder check done; approved by the user ("Leg los").

Reference for "looks right": no real-Voodoo reference is available. Glide frames are compared
with OpenGL-mode frames of the same scene; every difference is listed and shown to the user,
who decides whether it is the 3dfx look or a bug (my call, the user did not pick one).

## Intent

The PC version of Future Cop (a port of the PlayStation game) has three mission renderers in
the EXE: software, THRASH (EA's driver interface; our OpenGL renderer `thrash_gl.c` uses it)
and a built-in **3dfx Glide** path. On 1998 hardware the 3dfx version looked best. The user
wants that look in the port, as a renderer the player can choose permanently.

A wrapper such as dgVoodoo or nGlide cannot help here: the port runs recompiled code as a
native (64-bit) program and cannot load a 32-bit `glide2x.dll`. Instead the port implements
the Glide functions the game calls natively on OpenGL, the same way `thrash_gl.c` implements
THRASH.

The most likely reason the 3dfx path looks better (from the code, not yet confirmed by
screenshots): the Glide backend uploads textures as **ARGB1555** (Glide format 0xb, 5 bits
per colour, the PlayStation's own texture format), while the THRASH path receives
**ARGB4444** (4 bits per colour: visible banding). The Glide path also has its own colour
and alpha combine setup, chromakey transparency, gamma (`grGammaCorrectionValue`, fed from
0x511fc8) and a bilinear filter switch (0x48c154).

This renderer is also meant as the base for later work: texture replacement by hash (on
lossless 1555 data) and widescreen.

## Goals

- Missions render through the game's Glide path, implemented on OpenGL in our code.
- **Resolution (the user's decision):** rendered at the real window/screen resolution, sharp:
  vertices (float screen coordinates in the game's Glide resolution) are transformed to the
  real drawable size, never a low-resolution image scaled up. 4:3 pillarbox, as in OpenGL
  mode today.
- Textures in the game's original ARGB1555, converted losslessly for OpenGL.
- The game's own Glide settings work: bilinear filter switch, gamma, chromakey.
- All game modes work: Crime War and Precinct Assault, 1 and 2 players (split screen).
- Windows (MSYS2 MinGW) and Linux, the same code.

## Non-goals

- Widescreen, texture replacement, mipmaps, frame-rate limit: later projects.
- Changes to the THRASH renderer (beyond what sharing gl_present.c needs) or the software
  renderer.
- Menus, dialogs and videos: unchanged (they stay the 640x480 DirectDraw frames shown through
  `gl_present.c`). The Glide path never uses linear frame buffer calls (`grLfb*`), so nothing
  in the front end changes.
- Supporting real 3dfx hardware or loading any real `glide2x.dll`.
- The game's "3DFX Options" in its own dialogs: they work as the game drives them; no new UI.

## Anti-goals

- Anything derived from the EXE committed to the repository (CLAUDE.md rule). This includes
  code or data from the Glide SDK beyond what is needed to know the API (see "References").
- A change of the default: **OpenGL (THRASH) stays the default renderer** (user decision).
- Breaking the OpenGL or software renderer, or the Linux build.

## How the player chooses (user decision)

- Permanently, through `registry.txt`: REG_SZ `Group` under
  `HKLM\Software\Electronic Arts\Future Cop` (the game's own selector, read at startup in
  0x45ac80): `OpenGL` (default, THRASH), `3Dfx` (this renderer), `Software`.
- My call: the environment variable `FCOP_RENDERER` also takes `glide` (next to `opengl` and
  `software`) and overrides the registry for testing, as it does today (gdi_misc.c
  `RegQueryValueExA` forces the value the game reads).
- `thrash_wanted()` must treat `3Dfx` as "not THRASH"; a new `glide_wanted()` (or one
  function returning the renderer) answers for Glide. gdi_misc.c keeps writing `OpenGL` only
  when `Group` is missing.
- Fallback: when OpenGL cannot start (`glp_init` fails, or `FCOP_GL_FAIL=1`), `LoadLibraryA
  ("glide2x.dll")` returns 0. The builder confirms what the game does then (0x415be0 clears
  the Glide flag 0x511fa4 on failure; expected: software renderer) and that it is playable.

## How the game uses Glide (facts found so far)

- Loader 0x411680: `LoadLibraryA("glide2x.dll")` (string 0x4897b4), handle at 0x49fb80, then
  `GetProcAddress` for 121 names into a table 0x49fb84..0x49fd64 (one dword per name, names
  pushed just before each call). Called from the mission start code around 0x415be0.
- The Glide mode flag is the byte 0x511fa4 (set in 0x415be0 and the start-up selection
  0x45aa90/0x45ac80). About 25 game functions read it (e.g. 0x402110, 0x40afa0, 0x40bd00,
  0x4148e0, 0x414e80..0x415a30, 0x45a340), so the game prepares some data differently in
  Glide mode; the builder finds out what (texture preparation is the prime suspect).
- Per frame (0x40ca10): the game builds and depth-sorts one list of 0x38-byte primitive
  records, then calls the Glide backend 0x4393e0 when 0x511fa4 is set, else THRASH 0x456270,
  else the software renderer. **Both hardware paths draw the same primitive list.**
- Glide functions actually referenced (about 33): `grGlideInit`, `grGlideShutdown`,
  `grSstQueryBoards`, `grSstQueryHardware`, `grSstSelect`, `grSstWinOpen`, `grSstWinClose`,
  `grSstControl`, `grSstIdle`, `grBufferClear`, `grBufferSwap`, `grClipWindow`,
  `grDrawTriangle`, `guDrawTriangleWithClip`, `grDrawLine`, `grDrawPoint`, `grColorCombine`,
  `grAlphaCombine`, `grTexCombine`, `grConstantColorValue`, `grAlphaBlendFunction`,
  `grChromakeyMode`, `grChromakeyValue`, `grDitherMode`, `grDepthBufferMode`,
  `grGammaCorrectionValue`, `grTexSource`, `grTexMinAddress`, `grTexMaxAddress`,
  `grTexDownloadMipMap`, `grTexDownloadMipMapLevelPartial`, `grTexClampMode`,
  `grTexFilterMode`, `grTexMipMapMode`. All other names must still resolve (the loader stops
  at the first NULL: check) to safe no-op thunks that log once.
- Helpers: 0x43a300 queries the hardware (`grSstQueryHardware`; texture memory >= 0x1000
  and other fields choose a board type), 0x43a380 maps that to a mode count, 0x43a490 opens
  the window (`grSstWinOpen(hwnd, res, refresh, 0, 0, 2 colour buffers, 0 aux buffers)`,
  resolution code from a switch on the game's resolution index), 0x43a540 sets default
  states, 0x43a240/0x43a270 download textures: `grTexDownloadMipMap(tmu 0, address, evenOdd
  3, info)` with format 0xb (ARGB1555), aspect 3 (1:1), into texture memory between
  `grTexMinAddress` and `grTexMaxAddress`.
- No auxiliary (depth) buffer is requested: the game sorts by depth itself (as in THRASH).

## Design decisions

Made with the user:
- Implement Glide natively on OpenGL; selectable permanently through `Group` in registry.txt;
  OpenGL (THRASH) stays the default.
- Render at the real screen resolution by transforming vertices (see Goals).
- Claude builds (no Gemini; CLAUDE.md's Gemini notes are from the original author).

- The goal is the game's 3dfx path itself, not only better textures (the user chose Glide
  over improving THRASH after both options were laid out).

My calls (the user can override):
- Resolution list in Glide mode: the same full list as in OpenGL mode (320x240..1280x960),
  even if no real Voodoo offered it; report hardware and map resolution codes so the game
  accepts every index 0-9. A shorter Voodoo-era list only if the game cannot be made to
  accept more (tell the user).
- Gamma like a real Voodoo: applied to the whole mission picture, HUD included, as one pass
  at present time (not in the triangle shader).
- Texture cache shared by both renderers: move the content-hash/texture table code into one
  module used by `thrash_gl.c` and `glide_gl.c`, so texture replacement later works in both.
- New platform files `recomp/platform/glide_gl.c` + `glide.h`, hooked into `LoadLibraryA` /
  `GetProcAddress` in k32_proc.c next to `thrash_load`/`thrash_proc`; one thunk per
  function (reuse, like the THRASH fix in 538f616).
- Shared GL: same context, offscreen framebuffer, batching and frame read-back as THRASH
  (`gl_present.c`). Extend `gl_present` with what Glide needs (combine modes, chromakey,
  gamma) without changing THRASH output.
- Coordinate space: the resolution the game opens with `grSstWinOpen` (the `GR_RESOLUTION_*`
  code). The game's resolution setting decides it; rendering is always at the real size.
- Hardware we report: whatever board/memory answer makes the game offer the widest choice
  of resolutions and textures (probably a Voodoo 2 with generous texture memory). The
  builder verifies the offered modes and that the HUD is correct in each (the HUD scaling
  switches only cover resolution indices 0-9, see CLAUDE.md).
- Texture memory: emulate the TMU address space (a byte array from `grTexMinAddress` to
  `grTexMaxAddress`). Downloads write into it and mark the GL texture for that start address
  stale; `grTexSource(address, info)` picks (and if needed rebuilds) the GL texture for that
  address and format. 1555 -> RGBA8 exact (5-bit to 8-bit by bit replication). Keep the
  structure ready for texture replacement by content hash later.
- Combine modes: implement exactly the combinations the game sets (find them in 0x4393e0 /
  0x43a540) in the fragment shader; log once and fall back to "texture x vertex colour" for
  any other combination.
- Filtering: the game's `grTexFilterMode` setting, overridable with `FCOP_TEXFILTER` as in
  THRASH mode.
- Gamma: apply `grGammaCorrectionValue` in the shader or at present time, only to Glide
  frames.
- Write the behaviour into `docs/GLIDE_NOTES.md` (like docs/THRASH_NOTES.md), and the switch
  into CLAUDE.md.

## References

- Glide 2.x API (function signatures, `GrVertex` layout, `GrTexInfo`, enums such as
  `GR_RESOLUTION_*`, `GR_TEXFMT_*`, combine function/factor/local/other codes): the 3dfx Glide
  SDK, e.g. https://github.com/Danaozhong/3dfx-Glide-API (user's pointer). Use it only to look
  up the API; do not copy code (3dfx licence). Write our own `glide.h` with the few
  constants we need.
- The game's code (`build/recomp/gen/*.c` with the original instruction on every line,
  capstone, `data/functions.json`).
- `thrash_gl.c`, `gl_present.c`, docs/THRASH_NOTES.md as the model.

## Open points left to the builder

- What the other readers of 0x511fa4 do differently (texture preparation, HUD, 2D overlay).
- The `GrVertex` fields the game fills (x, y, oow, colour, sow/tow) and how z/oow map to depth
  and perspective-correct texturing.
- Which resolution codes the game passes for each resolution index, and the window close /
  reopen sequence on resolution change, pause dialog and Alt+Enter (`grSstControl`).
- What happens with split screen (two views: `grClipWindow`?).
- Exact `grSstQueryHardware` struct contents to report.

## Success check

- Mission 1 in Glide mode: same scene, HUD, radar and text as the OpenGL mode (side-by-side
  `--png` screenshots); textures visibly without 4444 banding; sharp at full screen height,
  4:3.
- 2-player split screen and Precinct Assault look right in Glide mode.
- Switching works: `Group` = `3Dfx` / `OpenGL` / `Software` in registry.txt, and
  `FCOP_RENDERER=glide`; a fresh registry.txt still defaults to OpenGL.
- Glide forced to fail (`FCOP_GL_FAIL=1`) leaves a playable game.
- Menus, dialogs, Esc pause bar, Alt+Enter, quit still work in Glide mode; OpenGL and
  software modes unchanged.
- Frame rate in Glide mode at least as good as OpenGL mode.
