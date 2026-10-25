# THRASH interface notes

How Future Cop drives a THRASH 3D driver, and what each call means, worked out (2026-10-25)
from the game's code and from the CD's `setup/thrash/opengla.dll` (EA's OpenGL 1.1 THRASH
driver, Watcom-built, base 0x60000000), plus probe runs (branch `spike/thrash-probe`) in
Crime War 1 player, Crime War 2 players (split screen) and Precinct Assault. This file
describes behaviour; it contains no code or data from either binary.

All functions are `__stdcall` (`_THRASH_<name>@<argbytes>`). Addresses `0x60xxxxxx` are in
opengla.dll, `0x4xxxxx` in FCopLAPD.exe.

## Selection and loading (game side)

- Startup (0x45acae) reads REG_SZ `Group` under `HKLM\Software\Electronic Arts\Future Cop`:
  `Software` -> software renderer, `3Dfx` -> built-in Glide, anything else -> THRASH, with
  REG_SZ `Thrash Driver` formatted as `"%sa.dll"` (0x4a3d30) and REG_DWORD `D3D Device`
  (0x4a3e34). Env `THRASH_DRIVERNAME` overrides the name (0x46b900; a leading `!` skips it).
- Mission start (0x415d41 -> 0x457030): `LoadLibraryA(name)`, then `GetProcAddress` for all 34
  exports into the table 0x512e2c..0x512eb0 (0x46b590). If a step fails the mission runs in
  software (0x415d4d clears 0x511fe4).
- Right after loading (0x46b869): `setstate(0x1f, 0x68)` (opengla ignores it: id out of
  range), then `about()`; the game requires `about()+8 == 0x68` (else error
  "THRASH_opendll") and sets a 3Dfx flag (0x4935dc) if `about()+0 == 'XFD3'` (0x33444658).
- Then (0x456fad..): `selectdisplay(0)`, `setstate(0x19, 0x5c)` (ignored by opengla),
  `setstate(0x13, 0x4a3d10)` (state callback struct, see below), `init()` (nonzero = ok),
  `setvideomode(a, 2, 0)`; then (0x456ff0) `setstate(2,0)`, `setstate(4,0)`, `window(2)`,
  `setstate(5,1)`; (0x4570d0) `is()` and `about()` again: the game reads `about()+0x3c`
  (count) and `about()+0x40` (pointer to `count` entries of 0x28 bytes, 1-based; an entry
  whose first dword >= 0x200 registers an extra display mode). A count of 0 skips this.
- Mission end (0x456f20): `restore()`.

## Calls seen (all three modes)

`about`, `is`, `selectdisplay`, `setstate`, `init`, `setvideomode`, `window`, `restore`,
`treset`, `talloc`, `tupdate`, `settexture`, `drawtri` (~1000/frame), `drawtrimesh`
(~200/frame), `drawline` (a few/frame), `flushwindow`, `pageflip`. Never called: `clip`,
`clearwindow`, `lockwindow`, `unlockwindow`, `readrect`, `writerect`, `drawquad*`,
`drawtristrip`, `drawtrifan`, `drawlinemesh`, `drawlinestrip`, `drawpoint*`, `idle`, `sync`.
In 2-player split screen `flushwindow` comes twice per `pageflip` (once per view); there are
no `clip` calls (the game positions each view's vertices itself).

## Functions (opengla behaviour)

- `about()` -> pointer to a 0x80-byte info struct. opengla: +0 'NEPO', +4 0x80, +8 0x68,
  +0x0c flags byte 0x0e, capability dwords, +0x3c 2 / +0x40 pointer to an all-zero table (so
  the game registers nothing), name "OpenGL 1.1" at +0x4c. Ours: +8 = 0x68, +0x3c = 0, the
  rest zero except a name; +0 must not be 'XFD3'.
- `is()` -> nonzero when the driver can run (opengla: opengl32.dll loads).
- `selectdisplay(n)` -> 0 (no-op).
- `init()` -> nonzero on success (loads opengl32, resolves functions).
- `setstate(0x13, p)`: copies 8 dwords from `p` (0x4a3d10) as the "state callback": +4 a
  function returning the game's window handle, +8 a function to register window-message
  handlers. opengla uses them to run its DirectDraw/WGL set-up on the window thread (messages
  0x464/0x465/0x468). Not needed natively: the game sets the DirectDraw display mode itself
  (log: "display mode WxH" before the first THRASH frame).
- `setvideomode(a, b, c)` (game passes (1,2,0) at load, (0,2,0) at mission start) -> 1.
  opengla: width/height from the primary DirectDraw surface's description (`dwWidth`,
  `dwHeight`), 16 or 24 bpp, creates the WGL context, then:
  `glHint(PERSPECTIVE_CORRECTION, NICEST)`, `glOrtho(0, width, height, 0, 0, 1)` (projection),
  default states (see table: 1=0, 2=1, 7=1, 3=0, 5=0, 6=1, 10=2, 11=1, 4=1, 14=0, 13=0),
  `glEnable(SCISSOR_TEST)` + `clip(0, 0, width-1, height-1)`,
  `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`, `glAlphaFunc(ALWAYS, 0)`,
  `glClearDepth(1)`, `glDepthFunc(LEQUAL)`, `glDepthRange(0, 1)`.
  **Coordinate space = the current DirectDraw display mode** (the game's Resolution setting).
- `window(n)`: `glDrawBuffer(table[n])`, table = {0, FRONT, BACK}; game uses 2 (back).
- `flushwindow()`: `glFlush`. `pageflip()`: `SwapBuffers`. `clearwindow()`: `glClear(COLOR |
  DEPTH)`. `idle()`: nothing. `sync(x)` -> 0.
- `restore()`: `treset()`, delete the context; -> 1.
- `talloc(w, h, fmt, palette, mips)` -> pointer to a texture object (opengla: a 0x24-byte host
  struct: +0 GL texture name (a running counter starting at 1), +4 w, +8 h, +0x0c mips
  (arg 5), +0x10 fmt, +0x14 palette (arg 4), +0x20 next in a list). The game treats it as an
  opaque handle and passes it to `tupdate`/`settexture`. Game uses (256, 256, 7, 0, 0).
- `tupdate(tex, pixels, palette)`: binds `tex`, sets wrap (state 13: nonzero = CLAMP, 0 =
  REPEAT) and filter (state 7: 1 = LINEAR mag, else NEAREST; state 11 = mipmap mode), texture
  env MODULATE, converts `pixels` and uploads with `glTexImage2D` (levels 0..mips).
  Formats: 2 = 8-bit paletted (`glColorTableEXT`), 3 = ARGB1555, 4 = RGB565, 5 = 24-bit,
  6 = 32-bit, **7 = ARGB4444** (16 bit: A = bits 12-15, R = 8-11, G = 4-7, B = 0-3; opengla
  expands a nibble x to x<<4, so 0xF -> 0xF0). The game re-uploads some textures during play
  (animation): ~30 `tupdate` per mission start, more while playing.
- `settexture(tex)` = `setstate(1, tex)`: tex != 0 -> enable 2D texturing and bind its name;
  0 -> disable texturing.
- `treset()`: delete all textures (`glDeleteTextures`), free the objects.
- `drawtri(v0, v1, v2)`: one triangle, `glBegin(TRIANGLES)`; with texturing on each vertex
  sends colour, texcoord (u, v), position; without, colour and position only.
- `drawtrimesh(n, verts, idx)`: `n` triangles; `verts` = vertex array, `idx` = `3n` u32
  indices into it; `glDrawElements(TRIANGLES, 3n, UNSIGNED_INT, idx)` with vertex/colour/
  texcoord arrays (stride 32).
- `drawline(v0, v1)`: one `GL_LINES` segment, with texcoords.
- `clip(x0, y0, x1, y1)`: scissor (inclusive corners). Not used by the game.

## Vertex (32 bytes)

| Offset | Type | Meaning |
|-|-|-|
| +0x00 | float | x, screen pixels (display-mode space) |
| +0x04 | float | y, screen pixels, 0 = top |
| +0x08 | float | z, depth 0..1 (opengla negates it for its `glOrtho(.., 0, 1)`, giving window depth = z) |
| +0x0c | float | 1/w (opengla ignores it: affine texturing; usable for perspective correction) |
| +0x10 | u32 | colour 0xAARRGGBB (opengla reorders to RGBA bytes); multiplies the texture (MODULATE) |
| +0x14 | u32 | unused by opengla |
| +0x18 | float | u, 0..1 |
| +0x1c | float | v, 0..1 |

## setstate ids (opengla handles 1..23; others return 0 and are ignored)

| id | value | GL |
|-|-|-|
| 1 | texture object or 0 | enable/bind or disable TEXTURE_2D (`settexture`) |
| 2 | 0 / 1 / 2 | cull off / on, front face CW / on, front face CCW. Game: 0 |
| 3 | 0xAARRGGBB | clear colour |
| 4 | 0 / 1 | DEPTH_TEST off / on. Game: 0 (painter's order) |
| 5 | 0 / 1 | DITHER. Game: 1 |
| 6 | 0 / 1 | shade model FLAT / SMOOTH. Game: 1 |
| 7 | 0 / 1 / other | texture filter for later uploads: 1 = LINEAR, else NEAREST. Game: 0, 1, 0xa0 (split screen) |
| 10 | 0 / 2 | BLEND + ALPHA_TEST off / on (blend SRC_ALPHA, ONE_MINUS_SRC_ALPHA) |
| 11 | 0 / 1 / 2 | mipmap filter mode for later uploads |
| 13 | 0 / 1 | texture wrap REPEAT / CLAMP for later uploads |
| 14, 15, 21, 22, 23 | | fog density, colour, mode, start, end (game does not set them) |
| 19 | pointer | state callback (above) |
| 0x194 | 0..9 | source blend factor (ignored by opengla; d3da.dll maps it): 0 ONE, 1 ZERO, 2 SRC_ALPHA, 3 ONE_MINUS_SRC_ALPHA, 4 DST_ALPHA, 5 ONE_MINUS_DST_ALPHA, 6 SRC_COLOR, 7 DST_COLOR, 8 ONE_MINUS_SRC_COLOR, 9 ONE_MINUS_DST_COLOR. Game: 0, 1, 2 |
| 0x195 | 0..9 | destination blend factor, same codes. Game pairs: (2,3) normal alpha, (0,0) additive glow, (1,8) darkening |

## Consequences for our implementation

- Map x, y from the display-mode size to the 4:3 viewport; depth from z.
- Blending is on by default (state 10 = 2); 0x194/0x195 choose the factors. opengla ignores
  them, so its glows came out dark; d3da.dll and our port honour them (additive glows match
  the software renderer).
- Coordinate space: the game's current resolution, entry `rd32(0x511e58)` of its table at
  0x48707c ({.., w at +4, h at +8, ..}, 20 bytes each). Not the DirectDraw display mode:
  with a saved 1600x1200 (index 10) the game still sets that display mode but renders at
  1280x960 (index 9), because the driver's mode list (`about()+0x3c/+0x40`) decides which
  indices are available in hardware mode. Our list leaves out 1600x1200 (HUD scaling bug).
- The hardware-mode Preferences dialog shows the driver's modes and a "Filtering" checkbox
  (setstate 7), no Fullscreen checkbox; missions then run windowed unless toggled.

## Widescreen and precise vertices

The walker 0x456270 converts the 2560x1920 display list to the game's resolution (1280x960 at
index 9, rounded). widescreen.c makes it pass the list's own units instead (text stays in game
pixels, drawn in separate calls), narrows the view before projection for widescreen and draws
the HUD with 4:3 viewports. The walker clips polygons itself (0x466300) against a rectangle set
by 0x469d90, which is scaled to match. Details: docs/superpowers/plans/2026-10-25-widescreen.md.
