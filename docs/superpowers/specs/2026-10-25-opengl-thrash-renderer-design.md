# OpenGL renderer for missions (THRASH on OpenGL) — design

Date: 2026-10-25. Status: reviewed by a builder check; awaiting the user's approval.

## Intent

Missions today are drawn by the game's own 1998 software renderer, on the CPU, into a 16-bit
DirectDraw surface. That is slow (13-22 FPS at 1600x1200 on a Ryzen 5800X3D), blurry when
scaled up to a modern monitor, and pixelated (no texture filtering).

The user wants, in their words: higher resolution, a sharper picture, texture filtering, and
"VIEL MEHR PERFORMANCE" (much more performance). The game already has a hardware 3D path
for 1998 accelerator cards; this project drives that path with our own OpenGL code so the
GPU does the work.

This is project 1 of 2. Project 2, true widescreen (16:9/16:10 with a wider view), comes
later and needs changes to the game's own projection, culling and HUD layout (see
"Widescreen findings" below). This project must not make project 2 harder.

## Goals

- Missions render through OpenGL on the GPU, at the real window/screen resolution (e.g.
  2560x1440 fullscreen), not at the game's chosen resolution scaled up.
- Bilinear texture filtering, on by default.
- Clearly faster than the software renderer: rendering is no longer the bottleneck at
  2560x1440 on the user's machine (RTX 4090), so missions run as fast as the game itself
  allows. (The game seems to limit itself: the probe saw about 45 pageflips per second
  while drawing nothing. Raising or removing that limit is a later, separate step, decided
  with the user; game speed may be tied to it.)
- OpenGL is the default. The software renderer stays as a fallback: switchable by a setting,
  and used automatically when OpenGL cannot start.
- Works on Windows (MSYS2 MinGW build) and Linux; the same code.

## Non-goals

- Widescreen with a wider field of view (project 2). Until then the picture is 4:3,
  pillarboxed (black bars left and right).
- Menus, dialogs and videos stay on the existing 640x480 DirectDraw path. They only have to
  keep working and look as they do today.
- Texture replacement / remastered assets (later; but see the texture-cache note below).
- The game's own frame-rate limit (later, separate step).
- The game's "3Dfx" (built-in Glide) path. Not used.
- Recompiling a THRASH driver DLL with x86c. Not needed (see "How the game uses THRASH").

## Anti-goals (would count as failure even if it "works")

- Anything derived from the EXE or the CD's DLLs committed to the repository (rule from
  CLAUDE.md). Reading opengla.dll for semantics is fine; copying its code or data is not.
- Breaking the software renderer or the Linux build.
- A setup step the player must do by hand (editing registry.txt) to get OpenGL.

## How the game uses THRASH (facts from reverse engineering and a probe run)

The probe (a throwaway logging implementation) is on local branch `spike/thrash-probe`,
file `recomp/platform/thrash_probe.c`; logs were taken in the first mission (Griffith Park),
including walking and shooting.

Selection, read once at startup (0x45acae):
- Registry `HKLM\Software\Electronic Arts\Future Cop` (the key itself, not `Settings`), REG_SZ
  value `Group`: `"Software"` = software renderer; `"3Dfx"` = built-in Glide path; any other
  value = THRASH. Then REG_SZ `Thrash Driver` (e.g. `"opengl"`) is formatted with `"%sa.dll"`
  into the driver file name (0x4a3d30); REG_DWORD `D3D Device` goes to 0x4a3e34. The
  environment variable `THRASH_DRIVERNAME` overrides the name (0x46b900).
- At mission start (0x415d41 -> 0x457030 -> 0x46b590) the game calls `LoadLibraryA(name)` and
  `GetProcAddress` for the 34 `_THRASH_*@N` exports (stdcall), storing them in a pointer table
  at 0x512e2c..0x512eb0. If THRASH fails to start, it falls back to software (0x415d4d).
- So the platform layer can implement THRASH natively: `LoadLibraryA` returns a pseudo module
  handle for the driver name, `GetProcAddress` returns thunks (`thunk_new`) to C functions,
  exactly like other imports. The probe did exactly this.
- `_THRASH_about` must return a guest pointer to an info struct. The game checks dword +8 ==
  0x68 (version, else error "THRASH_opendll") and dword +0 == 'XFD3' (sets a 3Dfx flag; we
  should not claim that). opengla.dll's layout (filled at 0x60001020): +0 'NEPO', +4 0x80,
  +8 0x68, +0xc flags byte 0x0e, then capability dwords and pointers to small tables, name
  string at +0x4c. Which fields the game reads beyond +0/+8 is not yet known.

Calls actually made (17 of 34), counts over ~740 frames:
- Setup: `about`, `is`, `selectdisplay(0)`, `setstate`, `init`, `setvideomode(a,b,c)`
  (seen (1,2,0) and (0,2,0)), `window(2)`, `restore(1,2,0)`, `treset`.
- Textures: `talloc(256,256,7,0,0)` (about 28 per mission, always 256x256, format 7, 16 bit;
  pixels like 0xf000/0xf222, probably ARGB4444) and `tupdate(handle, pixels, 0)` (also
  repeatedly during play: animated textures). `settexture(handle)` about 190 per frame.
- Drawing: `drawtri(v0,v1,v2)` about 1000 per frame, `drawtrimesh(n, verts, indices?)` about
  200 per frame, `drawline(v0,v1)` a few per frame.
- Per frame: `flushwindow`, `pageflip`. `setstate` about 45 per frame.
- Never called: `lockwindow`, `unlockwindow`, `writerect`, `readrect`, `clearwindow`, `clip`,
  quads, points, strips, fans. The HUD, radar and text are drawn as textured triangles and
  lines too. (Implement the unused ones as safe no-ops that log once.)

Vertex: 32 bytes, pointers passed per vertex: float x, float y (screen pixels), float z
(e.g. 0.99999), float 1/w (small, e.g. 1.5e-5), u32 colour ARGB, u32 (unknown: specular or
fog?), float u, float v (0..1). Vertices are already projected and clipped by the game.

setstate (id, value) pairs seen: (0x1f,0x68) (0x19,0x5c) (0x13, ptr 0x4a3d10) (2,0) (4,0) (5,1)
(6,1) (7,0/1) (0x194, 0/1/2) (0x195, 0/3/8). Meanings are to be read from opengla.dll's
`_THRASH_setstate` (expected: blend/alpha modes, texture filtering, depth, culling,
fog). The game's chosen resolution index also matters: see the note under "Open points".

## Design decisions

Made with the user:
- OpenGL is the default renderer; software stays as fallback, selectable by a setting
  (registry.txt value or environment variable), and used automatically if OpenGL fails.
- Order: renderer first (this document), widescreen second.
- Picture fills the screen height at 4:3 until project 2.
- Frame rate: this project only removes rendering as the bottleneck; the game's own frame
  limit is a later step.
- Who builds: Claude implements; Codex (`codex exec`, the user's ChatGPT plan, installed on
  this machine) takes well-specified sub-tasks and gives a second, independent review. Ask
  the user before each Codex run that writes files. No Gemini (the Gemini notes in CLAUDE.md
  are from the repository's original author).

My calls (the user can override):
- All game modes must work in OpenGL mode: Crime War and Precinct Assault, 1 and 2 players
  (split screen), all missions. Software is only the fallback. Probe split screen and
  Precinct Assault the same way as mission 1 before relying on the call list above.
- The `_THRASH_about` struct (and any tables it points to) is built entirely by our code in
  guest memory; the port never reads opengla.dll at run time. Work out which fields the game
  reads from it.
- Headless tests: in OpenGL mode `plat_test.py` uses a hidden real window (needs a desktop
  session, no new library) and reads the GL frame back for `--png`.
- The game's Resolution setting keeps setting the window size in windowed mode (as today);
  rendering is always at the real window/screen size.
- Implement the THRASH functions natively in C on OpenGL in a new platform file (e.g.
  `recomp/platform/thrash_gl.c`), hooked into `LoadLibraryA`/`GetProcAddress` in k32_proc.c.
  Use `opengla.dll` (CD: `setup/thrash/`) as the reference for semantics, read with capstone;
  never copy from it.
- Default on: the platform makes the game see THRASH without the player editing anything,
  e.g. registry defaults created in gdi_misc.c (`Group`, `Thrash Driver`), or by answering
  the registry query, with the fallback setting turning it off. The builder picks the
  mechanism; it must survive the game saving its settings on exit.
- GL context through SDL2 (already the window/input layer), loading GL functions at run time
  (SDL_GL_GetProcAddress), no new libraries. Target OpenGL 3.3 core with a simple shader
  path, written so a later GLES 3 (Android) port is easy. Batch triangles per texture/state
  change instead of one draw call per triangle (about 1200 draws per frame otherwise).
- Coordinates: the game's screen-space vertices are mapped from the game's resolution to the
  window with an orthographic transform, keeping 4:3 and centring (pillarbox). Depth from
  z or 1/w (whichever opengla uses), perspective-correct texturing via 1/w.
- Textures: convert ARGB4444 (or whatever format 7 really is) to RGBA8 on upload; bilinear
  filtering by default, a setting for nearest (the old pixel look). Keep a per-handle texture
  table; structure it so textures can later be looked up by content hash for replacement.
- One presentation path per window: today menus are shown through SDL_Renderer (u32_win.c
  `win_show`). With OpenGL on, the builder must make menus/dialogs/videos and missions share
  one GL context (e.g. draw the 16-bit menu frames as a GL texture), because mixing
  SDL_Renderer and raw GL on one window is unreliable. Fullscreen/windowed and Alt+Enter keep
  working.
- Settings live where the other port settings live (registry.txt / environment variables,
  documented in CLAUDE.md): renderer (opengl/software), texture filter (linear/nearest).
- Testing: headless `plat_test.py` must still produce frames for `--png` in OpenGL mode (read
  back the GL frame), so screenshots can be compared with the software renderer.

## Widescreen findings (for project 2, recorded here so nothing blocks it)

With the resolution table entry for 1280x960 patched to 1280x720 (software renderer), the game
keeps the 4:3 horizontal view and crops top and bottom; the player sits at the bottom edge and
the bottom HUD row is off screen. Not distorted. Project 2 needs: projection centre/scale based
on height (Hor+), culling widened to the new width, HUD anchoring. Also known: the game's HUD
scaling switches only cover resolution indices 0-9 (0x40cd3f, 0x410552), see CLAUDE.md. In
THRASH mode the builder should check which coordinate space the game uses (which resolution
index `setvideomode` gets) and pick one where the HUD switches are correct.

## Open points left to the builder

- Exact meaning of each setstate id/value, the about-struct fields the game reads, the vertex
  dword at +0x14, `drawtrimesh` argument layout, texture format 7, and `setvideomode`'s
  arguments: read opengla.dll (and d3da.dll as a second reference) and confirm in game.
- Whether the game in THRASH mode uses its "Resolution" setting for the vertex coordinate
  space, and which index to choose (any index <= 9 avoids the 1600x1200 HUD bug).
- Frame pacing: vsync on; do not change the game's own timing in this project.
- How the fallback detects "OpenGL cannot start" (context creation failure, missing GL 3.3).

## Success check

- Mission 1 starts and plays in OpenGL mode: same scene, HUD, radar and text as the software
  renderer (side-by-side screenshots), filtered textures, 4:3 centred at full screen height.
- At 2560x1440 fullscreen on the user's machine the frame rate equals what the game runs at
  with drawing disabled (measured with an FPS log line in both cases): the GPU path costs
  nothing noticeable. Compare with the software renderer's FPS.
- 2-player split screen and a Precinct Assault match look right in OpenGL mode.
- Software fallback works: setting switched, and with OpenGL forced to fail.
- Menus, dialogs, videos, Esc pause bar, Alt+Enter, quitting all still work in both modes.
- Linux build still compiles and runs (cannot be tested on this machine; keep code portable).
