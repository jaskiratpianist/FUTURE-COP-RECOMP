# Glide interface notes

How Future Cop drives its built-in 3dfx Glide 2.x renderer, worked out (2026-10-25) from the
game's code and from a probe run (throwaway branch `spike/glide-probe`: every export logs its
arguments) of the first Crime War mission at resolution index 9. API constants are from the
Glide 2.x SDK headers. This file describes behaviour; it contains no code or data from the
EXE or the SDK.

All functions are `__stdcall` (`_gr<Name>@<argbytes>`, `_gu<Name>@<argbytes>`).

## Selection and loading

- `Group` = `3Dfx` (REG_SZ, `HKLM\Software\Electronic Arts\Future Cop`) selects Glide at
  start-up (0x45ac80 / 0x45aa90). The Glide mode flag is the byte 0x511fa4; about 25 game
  functions read it and prepare their data for Glide themselves (e.g. textures in 1555).
- Loader 0x411680: `LoadLibraryA("glide2x.dll")` (handle at 0x49fb80), then `GetProcAddress`
  for 121 names into 0x49fb84..0x49fd64. **Any NULL makes the whole load fail**, so every name
  must resolve. On failure the game clears 0x511fa4 (0x415be0) and uses the software renderer.
- The loader runs at start-up (the game opens a 640x480 window once, sets its defaults, closes
  it again) and at every mission start.

## Call sequence

Start-up and mission start (0x43a5d0, 0x43a540):

1. `grSstQueryBoards(&n)`, `grGlideInit()`, `grSstQueryHardware(&hw)` (0x43a300), twice.
2. `grSstSelect(0)`, `grSstWinOpen(hwnd, res, 3 /* 60 Hz */, 0 /* ARGB */, 0 /* origin upper
   left */, 2 colour buffers, 0 aux buffers)`; no depth buffer: the game sorts by depth.
3. `grDepthBufferMode(0)`, `grBufferClear(0, 0, 0)`, `grBufferSwap(0)`, `grBufferClear` again.
4. Defaults (0x43a540): `grDitherMode(0)`, `grTexClampMode(0, CLAMP, CLAMP)`,
   `grTexMipMapMode(0, DISABLE, FALSE)`, `grTexFilterMode(0, f, f)` with f = 1 (BILINEAR) when
   the game's filtering byte 0x48c154 is set, else 0 (POINT), `grChromakeyMode(1)`,
   `grChromakeyValue(0)` (black), `grSstQueryHardware`, `grGammaCorrectionValue(g)` with
   g = 0x511fc8 * k - c (1.25 seen).
5. Textures (0x43a270), per 256x256 texture: `grSstIdle`, `grTexMinAddress(0)`,
   `grTexMaxAddress(0)` (address + size must fit), `grTexDownloadMipMap(0, addr, 3 /* both */,
   &info)`, `grSstIdle`. Addresses 0, 0x20000, 0x40000, ...: one texture every 128 KB.
6. `grSstWinClose()` before reopening (resolution change) and at mission end (0x43a640, then
   `grSstIdle`, `grGlideShutdown`).

Per frame (backend 0x4393e0, called from the frame function 0x40ca10 with the same sorted
primitive list THRASH and the software renderer get):

- `grClipWindow(x0, 0, x1, y1)`: the game's width x height, except index 9 (1280x960), which
  clips to 1280x1024 (the Glide mode it opens). In 2-player split screen the backend runs once
  per view with x0/x1 the half (0x439406: half width when 0x511f54 is set).
- `grTexCombine(0, LOCAL, NONE, LOCAL, NONE, FALSE, FALSE)` (texture passes through).
- Per primitive: `grConstantColorValue`, `grTexSource`, `grColorCombine`, `grAlphaCombine`,
  `grAlphaBlendFunction` when they change, then `grDrawTriangle` or `guDrawTriangleWithClip`
  (same vertices; we draw both the same way, the clip window clips), `grDrawLine`,
  `grDrawPoint` (not seen in mission 1).
- No `grBufferClear` per frame (the scene covers the screen). `grBufferSwap(1)` (0x4393d0) at
  the end.
- Animated textures: `grTexDownloadMipMapLevelPartial(0, 0x1a0000, LOD 256, LOD 256, aspect 3,
  fmt 0xb, evenOdd 3, data, row0 0xd0, row1 0xff)` (0x43a240) every few frames.
- `grSstControl` (0x45a340, window activation) was not seen in mission 1.

## Hardware answer and resolutions

- `grSstQueryHardware` (GrHwConfiguration): +0 num_sst, then per board +4 type (0 = Voodoo),
  +8 fbRam (MB), +0xc fbiRev, +0x10 nTexelfx, +0x14 sliDetect, +0x18/+0x1c tmu 0 rev / RAM.
- 0x43a380 turns the answer into the highest resolution index offered: type 0 or 3 with fbRam
  <= 2 -> 5, <= 4 -> 7, <= 6 -> 9, more -> 10. **Report fbRam 6**: index 10 (1600x1200) has
  the game's HUD bug (CLAUDE.md). (0x43a300 also reads fbiRev: >= 0x1000 -> 5, <= 0x100 -> 1,
  else 4; any positive result is fine.)
- 0x43a490 maps the game's resolution index to a `GR_RESOLUTION_*` code:
  4 -> 7 (640x480), 5 -> 8 (800x600), 6 -> 9 (960x720), 7 -> 0xc (1024x768),
  9 -> 0xd (1280x1024), 10 -> 0xe (1600x1200); **0-3 and 8 (1120x840) fail** (the game's own
  switch returns before `grSstWinOpen`), so Glide mode offers 640x480 .. 1280x960 without
  1120x840.
- The game's coordinate space is the Glide screen it opened. At index 9 that is 1280x1024:
  the HUD is laid out for 1024 lines (its bottom row sits below y 960) and the 3D view fills
  the whole screen. So the port draws each Glide frame in the aspect of that screen with square
  pixels (5:4 at 1280x1024, 4:3 for the others).

## Vertices (GrVertex, 60 bytes)

`x, y, z, r, g, b, ooz, a, oow` (floats, +0..+0x20), then tmuvtx[0] `sow, tow, oow` (+0x24,
+0x28, +0x2c) and tmuvtx[1] (unused). Seen: x, y in game pixels (fractions; may lie outside
the screen: the clip window clips), z = 1, ooz 0 or 255, oow = 1 and tmu oow = 1 (affine
texturing, the PlayStation look), r, g, b, a 0..255 (the game doubles PlayStation colours and
clamps at 255), sow/tow in texels of the 256x256 texture (0..256).

## Combine and blend states seen

Glide 2 codes: function 1 LOCAL, 3 SCALE_OTHER, 4 SCALE_OTHER_ADD_LOCAL; factor 0 ZERO,
1 LOCAL; local 0 ITERATED, 1 CONSTANT; other 1 TEXTURE, 2 CONSTANT.

| call | arguments (func, factor, local, other, invert) | result |
|------|-----|-----|
| `grColorCombine` | 3, 1, 0, 1, 0 | texture rgb x vertex rgb (most triangles) |
| `grColorCombine` | 3, 1, 1, 1, 0 | texture rgb x constant rgb |
| `grColorCombine` | 1, 0, 1, 2, 0 | constant rgb |
| `grColorCombine` | 1, 0, 0, 2, 0 | vertex rgb |
| `grAlphaCombine` | 1, 0, 1, 2, 0 | constant alpha |
| `grAlphaCombine` | 1, 0, 0, 2, 0 | vertex alpha |
| `grAlphaCombine` | 3, 1, 1, 1, 0 | texture alpha x constant alpha |
| `grAlphaCombine` | 4, 1, 1, 1, 0 | texture alpha x constant alpha + constant alpha (clamped) |

`grConstantColorValue` is ARGB (0xff000000, 0xffefefef, 0x80c6c6c6, ...).
`grAlphaBlendFunction(rgb_src, rgb_dst, a_src, a_dst)`: (1, 5) SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
(0, 5) ZERO / ONE_MINUS_SRC_ALPHA (darkening, e.g. shadows), (4, 4) ONE / ONE (additive);
alpha factors are (4, 0) and do not matter (no destination alpha).

Chromakey: on, key 0 (black). On a Voodoo the key is compared with the colour combine unit's
"other" input (the texel colour for the texture modes), before blending: texels whose colour
is 0x0000 or 0x8000 in 1555 (rgb black) are not drawn; the game's opaque near-black is 0x8001.
The port improves on this with filtered textures: the four texels are filtered with keyed
ones left out and their share becomes a soft alpha edge (no stair steps, no dark fringe;
the user found the hard texel edges too sharp).

## Textures

`GrTexInfo` {smallLod, largeLod, aspectRatio, format, data}: always LOD 0 (256) / 0 / aspect 3
(1:1) / format 0xb ARGB1555, one level. 1555: A = bit 15, R = bits 10-14, G = 5-9, B = 0-4;
worked pixels: `0x7c1f` -> R ff G 00 B ff A 00, `0xfc00` -> ff 00 00 ff (5 bits x -> x << 3 |
x >> 2). `grTexSource(0, addr, 3, &info)` selects the texture by its start address.

## Gamma

`grGammaCorrectionValue(g)`: Glide builds the scanout table out = in^(1/g) for the whole
picture (HUD included). The game passes 1.25 at the default brightness.

## Compared with OpenGL (THRASH) mode

User's verdict (2026-10-25, side-by-side shots): the dithering-like pattern on textures in
OpenGL mode is gone in Glide mode and textures look smoother. Cause: THRASH receives the
textures from the game already reduced to ARGB4444 (dithered, 4 bits per colour); Glide gets
the game's ARGB1555 (5 bits, the PlayStation's own format). Other differences seen: gamma 1.25
(brighter), the game's own Glide HUD layout, fainter translucent light cones in Precinct
Assault (not yet judged).

## Widescreen

At the 1280x960 setting the game opens a 1280x1024 Glide screen and stretches its 4:3 picture
to it (y * 1.0667, constant 0x48466c in the walker 0x4393e0); type-6 text keeps unscaled pixel
y, so it sits about 60 px above its icons there (the game's own). Widescreen stretches the
whole screen like THRASH and draws the HUD pass with a 4:3 viewport, not 5:4. Details:
docs/superpowers/plans/2026-10-25-widescreen.md.
